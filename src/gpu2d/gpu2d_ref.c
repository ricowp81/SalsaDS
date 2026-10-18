/*
 * SalsaDS — motores 2D: referencia píxel por píxel (Fase 2.1).
 *
 * Escrita para ser obvia, no rápida: por cada píxel pregunta a cada capa, en el orden de prioridad de la DS,
 * quién es el primero que no es transparente, y aplica ventanas, mezclas y brillo con las reglas de GBATEK
 * (corregidas donde melonDS 1.1 mostró otra cosa: ver docs/fase2.md). No comparte código con gpu2d_compose.c
 * (ni la decodificación del OAM ni el cálculo de direcciones): sirve para verificarlo.
 *
 * Los colores viajan como BGR555 de 5 bits por canal; el color final se convierte a RGBA5551 al salir.
 */
#include <string.h>

#include <stdlib.h>
#include "gpu2d/gpu2d.h"
#include "gpu2d/gpu2d_fx.h"
#include "util/endian.h"

typedef struct {
	int v;       /* 1 = opaco */
	uint16_t c;  /* BGR555 */
} px_t;

typedef struct {
	int idx, x, y, w, h, bw, bh;
	int aff, dbl, mode, mos, c256, tile, prio, pal, flipx, flipy;
	int pa, pb, pc, pd;
	int alpha;  /* a2 bits 12-15 (bitmap) */
} robj;

typedef struct {
	const gpu2d_engine *e;
	int isb;
	uint16_t regs[G2D_NREGS];
	uint16_t pal[512];
	uint8_t oam[1024];
	int32_t ref[2][2];   /* punto de referencia interno de BG2 y BG3 (X, Y), 28 bits con signo */
	int32_t mref[2][2];  /* el de arriba, fijado al empezar el grupo de mosaico vertical */
	int winv[2];         /* bandera vertical de cada ventana */
	uint32_t dc;         /* DISPCNT de la línea */
} rs_t;

static const uint8_t obj_dims[3][4][2] = {
	{ { 8, 8 }, { 16, 16 }, { 32, 32 }, { 64, 64 } },
	{ { 16, 8 }, { 32, 8 }, { 32, 16 }, { 64, 32 } },
	{ { 8, 16 }, { 8, 32 }, { 16, 32 }, { 32, 64 } },
};

static inline const uint8_t *vram_of(const gpu2d_engine *e)
{
	return (const uint8_t *)(uintptr_t)e->vram_ea;
}

/* Lecturas de páginas sin banco (diagnóstico): melonDS espeja alguna de esas zonas (por ejemplo, la VRAM de OBJ de A
 * repite sus 32 KB), la DS real no está documentada; SalsaDS lee ceros. */
unsigned gpu2d_ref_unmapped_reads;

/* Lectura de un byte de la vista de fondos (con envoltura) */
static uint8_t bg8(const gpu2d_engine *e, uint32_t off)
{
	off &= e->bgpages * 0x4000u - 1u;
	const uint32_t base = e->bgpg[off >> 14];
	if (base == G2D_UNMAPPED) {
		gpu2d_ref_unmapped_reads++;
		return 0;
	}
	return vram_of(e)[base + (off & 0x3FFF)];
}

static uint16_t bg16(const gpu2d_engine *e, uint32_t off)
{
	return (uint16_t)(bg8(e, off) | (bg8(e, off + 1) << 8));
}

static uint8_t obj8(const gpu2d_engine *e, uint32_t off)
{
	off &= e->objpages * 0x4000u - 1u;
	const uint32_t base = e->objpg[off >> 14];
	if (base == G2D_UNMAPPED) {
		gpu2d_ref_unmapped_reads++;
		return 0;
	}
	return vram_of(e)[base + (off & 0x3FFF)];
}

static uint16_t obj16(const gpu2d_engine *e, uint32_t off)
{
	return (uint16_t)(obj8(e, off) | (obj8(e, off + 1) << 8));
}

/* Color de la paleta extendida: `base` es el desplazamiento de la ranura (o G2D_UNMAPPED), `idx` el color. */
static uint16_t ext_color(const gpu2d_engine *e, uint32_t base, uint32_t idx)
{
	if (base == G2D_UNMAPPED)
		return 0;
	return (uint16_t)(rd16le(vram_of(e) + base + idx * 2u) & 0x7FFFu);
}

static int32_t sext28(uint32_t v)
{
	return (int32_t)(v << 4) >> 4;
}

static uint32_t reg32(const rs_t *s, int r)
{
	return (uint32_t)s->regs[r] | ((uint32_t)s->regs[r + 1] << 16);
}

/* ---- cambios -------------------------------------------------------------------------------------- */

static void reload_ref(rs_t *s, int b)
{
	const int base = b ? G2D_R_BG3PA : G2D_R_BG2PA;

	s->ref[b][0] = sext28(reg32(s, base + 4));
	s->ref[b][1] = sext28(reg32(s, base + 6));
}

static void apply_change(rs_t *s, const gpu2d_change *c)
{
	if (c->reg < G2D_NREGS) {
		s->regs[c->reg] = (uint16_t)c->value;
		if (c->reg >= G2D_R_BG2PA + 4 && c->reg < G2D_R_BG2PA + 8)
			reload_ref(s, 0);
		else if (c->reg >= G2D_R_BG3PA + 4 && c->reg < G2D_R_BG3PA + 8)
			reload_ref(s, 1);
	} else if (c->reg == G2D_CHG_PAL) {
		s->pal[(c->value >> 16) & 511] = (uint16_t)(c->value & 0x7FFFu);
	} else if (c->reg == G2D_CHG_OAM) {
		const uint32_t i = (c->value >> 16) & 511u;

		wr16le(s->oam + i * 2, (uint16_t)c->value);
	}
}

/* ---- fondos --------------------------------------------------------------------------------------- */

enum { BG_OFF, BG_TEXT, BG_AFFINE, BG_EXT16, BG_BMP8, BG_BMPD, BG_LARGE, BG_3D };

static int bg_type(const rs_t *s, int b)
{
	const uint32_t dc = s->dc;
	const uint32_t mode = G2D_DC_BGMODE(dc);
	const uint32_t cnt = s->regs[G2D_R_BGCNT + b];

	if (!(dc & G2D_DC_BG(b)))
		return BG_OFF;
	if (b == 0 && !s->isb && ((dc & G2D_DC_3D) || mode == 6))
		return BG_3D;
	if (b < 2)
		return mode >= 6 ? BG_OFF : BG_TEXT;
	if (mode == 6)
		return (b == 2 && !s->isb) ? BG_LARGE : BG_OFF;
	if (mode == 7)
		return BG_OFF;
	int kind;  /* 0 texto, 1 afín, 2 extendido */
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

/* Cuadro de mosaico de fondos: coordenadas de pantalla redondeadas hacia abajo al múltiplo del bloque. */
static void bg_mosaic(const rs_t *s, int b, int x, int y, int *xs, int *ys)
{
	*xs = x;
	*ys = y;
	if (s->regs[G2D_R_BGCNT + b] & G2D_BGCNT_MOSAIC) {
		const int mh = (s->regs[G2D_R_MOSAIC] & 15) + 1, mv = ((s->regs[G2D_R_MOSAIC] >> 4) & 15) + 1;

		*xs = x - x % mh;
		*ys = y - y % mv;
	}
}

static px_t bg_text(const rs_t *s, int b, int x, int y)
{
	const gpu2d_engine *e = s->e;
	const uint32_t dc = s->dc, cnt = s->regs[G2D_R_BGCNT + b];
	const uint32_t size = G2D_BGCNT_SIZE(cnt);
	const int wide = size & 1, tall = size >> 1;
	int xs, ys;
	px_t r = { 0, 0 };

	bg_mosaic(s, b, x, y, &xs, &ys);
	const int px = (xs + (s->regs[G2D_R_BGHOFS + 2 * b] & 0x1FF)) & (wide ? 511 : 255);
	const int py = (ys + (s->regs[G2D_R_BGVOFS + 2 * b] & 0x1FF)) & (tall ? 511 : 255);
	const int tx = px >> 3, ty = py >> 3;
	const uint32_t scr_off = s->isb ? 0 : G2D_DC_SCR_OFF(dc), char_off = s->isb ? 0 : G2D_DC_CHAR_OFF(dc);
	const uint32_t block = (uint32_t)(tx >> 5) + (uint32_t)(ty >> 5) * (wide ? 2u : 1u);
	const uint32_t map = scr_off + G2D_BGCNT_SCR(cnt) + block * 0x800u + (uint32_t)((ty & 31) * 32 + (tx & 31)) * 2u;
	const uint16_t me = bg16(e, map);
	int ix = px & 7, iy = py & 7;

	if (me & 0x400)
		ix = 7 - ix;
	if (me & 0x800)
		iy = 7 - iy;
	const uint32_t cbase = char_off + G2D_BGCNT_CHAR(cnt);
	if (G2D_BGCNT_256(cnt)) {
		const int c = bg8(e, cbase + (uint32_t)(me & 1023) * 64u + (uint32_t)(iy * 8 + ix));

		if (!c)
			return r;
		r.v = 1;
		if (dc & G2D_DC_BGEXTPAL) {
			uint32_t slot = (uint32_t)b;

			if (b == 0 && (cnt & G2D_BGCNT_SLOT2))
				slot = 2;
			if (b == 1 && (cnt & G2D_BGCNT_SLOT2))
				slot = 3;
			r.c = ext_color(e, e->extbg[slot], (uint32_t)((me >> 12) & 15) * 256u + (uint32_t)c);
		} else {
			r.c = s->pal[c];
		}
		return r;
	}
	const uint8_t byte = bg8(e, cbase + (uint32_t)(me & 1023) * 32u + (uint32_t)(iy * 4 + (ix >> 1)));
	const int c = (ix & 1) ? byte >> 4 : byte & 15;

	if (!c)
		return r;
	r.v = 1;
	r.c = s->pal[((me >> 12) & 15) * 16 + c];
	return r;
}

/* Rotación y escala (afín, extendido de 16 bits, mapas de bits y el grande). */
static px_t bg_affine(const rs_t *s, int type, int b, int x, int y)
{
	const gpu2d_engine *e = s->e;
	const uint32_t dc = s->dc, cnt = s->regs[G2D_R_BGCNT + b];
	const int rb = b - 2;  /* 0 = BG2, 1 = BG3 */
	const int pbase = b == 3 ? G2D_R_BG3PA : G2D_R_BG2PA;
	const int pa = (int16_t)s->regs[pbase], pc = (int16_t)s->regs[pbase + 2];
	const uint32_t size = G2D_BGCNT_SIZE(cnt);
	int w, h, xs, ys;
	px_t r = { 0, 0 };

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
	bg_mosaic(s, b, x, y, &xs, &ys);
	(void)ys;
	const int32_t *ref = (cnt & G2D_BGCNT_MOSAIC) ? s->mref[rb] : s->ref[rb];
	int sx = (int)((ref[0] + (int64_t)pa * xs) >> 8), sy = (int)((ref[1] + (int64_t)pc * xs) >> 8);

	if (cnt & G2D_BGCNT_WRAP) {
		sx &= w - 1;
		sy &= h - 1;
	} else if (sx < 0 || sx >= w || sy < 0 || sy >= h) {
		return r;
	}
	const uint32_t scr_off = s->isb ? 0 : G2D_DC_SCR_OFF(dc), char_off = s->isb ? 0 : G2D_DC_CHAR_OFF(dc);
	const uint32_t cbase = char_off + G2D_BGCNT_CHAR(cnt);
	switch (type) {
	case BG_AFFINE: {
		const uint32_t sb = scr_off + G2D_BGCNT_SCR(cnt);
		const uint32_t tile = bg8(e, sb + (uint32_t)((sy >> 3) * (w >> 3) + (sx >> 3)));
		const int c = bg8(e, cbase + tile * 64u + (uint32_t)((sy & 7) * 8 + (sx & 7)));

		if (c) {
			r.v = 1;
			r.c = s->pal[c];
		}
		return r;
	}
	case BG_EXT16: {
		const uint32_t sb = scr_off + G2D_BGCNT_SCR(cnt);
		const uint16_t me = bg16(e, sb + (uint32_t)((sy >> 3) * (w >> 3) + (sx >> 3)) * 2u);
		int ix = sx & 7, iy = sy & 7;

		if (me & 0x400)
			ix = 7 - ix;
		if (me & 0x800)
			iy = 7 - iy;
		const int c = bg8(e, cbase + (uint32_t)(me & 1023) * 64u + (uint32_t)(iy * 8 + ix));

		if (c) {
			r.v = 1;
			if (dc & G2D_DC_BGEXTPAL)
				r.c = ext_color(e, e->extbg[b], (uint32_t)((me >> 12) & 15) * 256u + (uint32_t)c);
			else
				r.c = s->pal[c];
		}
		return r;
	}
	case BG_BMP8: {
		const int c = bg8(e, G2D_BGCNT_SCR_BMP(cnt) + (uint32_t)(sy * w + sx));

		if (c) {
			r.v = 1;
			r.c = s->pal[c];
		}
		return r;
	}
	case BG_BMPD: {
		const uint16_t v = bg16(e, G2D_BGCNT_SCR_BMP(cnt) + (uint32_t)(sy * w + sx) * 2u);

		if (v & 0x8000u) {
			r.v = 1;
			r.c = v & 0x7FFFu;
		}
		return r;
	}
	default: {  /* BG_LARGE */
		const int c = bg8(e, (uint32_t)(sy * w + sx));

		if (c) {
			r.v = 1;
			r.c = s->pal[c];
		}
		return r;
	}
	}
}

/* ---- OBJ ---------------------------------------------------------------------------------------- */

static int decode_obj(const rs_t *s, int i, robj *o)
{
	const uint8_t *oam = s->oam;
	const uint16_t a0 = rd16le(oam + i * 8), a1 = rd16le(oam + i * 8 + 2), a2 = rd16le(oam + i * 8 + 4);
	const int aff = (a0 >> 8) & 1, shape = a0 >> 14, size = a1 >> 14;

	if (!aff && (a0 & 0x200))
		return 0;
	if (((a0 >> 10) & 3) == 3 && (a2 >> 12) == 0) /* mapa de bits con alfa 0: no se dibuja (melonDS 1.1) */
		return 0;
	memset(o, 0, sizeof *o);
	o->idx = i;
	o->aff = aff;
	o->dbl = aff && (a0 & 0x200);
	o->mode = (a0 >> 10) & 3;
	o->mos = (a0 >> 12) & 1;
	o->c256 = (a0 >> 13) & 1;
	/* La forma 3 está prohibida; melonDS (la referencia) la dibuja de 8×8 sea cual sea el tamaño. */
	o->w = shape == 3 ? 8 : obj_dims[shape][size][0];
	o->h = shape == 3 ? 8 : obj_dims[shape][size][1];
	o->bw = o->w << o->dbl;
	o->bh = o->h << o->dbl;
	o->y = a0 & 255;
	o->x = a1 & 511;
	if (o->x >= 256)
		o->x -= 512;
	o->tile = a2 & 1023;
	o->prio = (a2 >> 10) & 3;
	o->pal = a2 >> 12;
	o->alpha = a2 >> 12;
	if (aff) {
		const int g = (a1 >> 9) & 31;

		o->pa = (int16_t)rd16le(oam + g * 32 + 6);
		o->pb = (int16_t)rd16le(oam + g * 32 + 14);
		o->pc = (int16_t)rd16le(oam + g * 32 + 22);
		o->pd = (int16_t)rd16le(oam + g * 32 + 30);
	} else {
		o->flipx = (a1 >> 12) & 1;
		o->flipy = (a1 >> 13) & 1;
	}
	return 1;
}

typedef struct {
	int v;
	uint16_t c;
	int semi;    /* modo semitransparente (a0 bits 10-11 = 1) */
	int balpha;  /* OBJ de mapa de bits: a2 bits 12-15 (0..15), o -1 */
	int win;     /* píxel de un OBJ de ventana */
	int prio;    /* prioridad del OBJ que lo puso */
	int mos;     /* el OBJ que lo puso tiene mosaico */
	int inbox;   /* el píxel cae dentro del OBJ (aunque sea transparente) */
} opx_t;

static opx_t obj_pixel(const rs_t *s, const robj *o, int x, int line)
{
	const gpu2d_engine *e = s->e;
	const uint32_t dc = s->dc;
	opx_t r = { 0, 0, 0, -1, 0, 0, 0, 0 };
	int dy = (line - o->y) & 255;

	if (x - o->x < 0 || x - o->x >= o->bw || dy >= o->bh)
		return r;
	if (o->mos) { /* mosaico vertical: por OBJ, con bloques de pantalla; el horizontal se aplica a la línea de OBJ */
		const int mv = ((s->regs[G2D_R_MOSAIC] >> 12) & 15) + 1;

		/* (línea del principio del bloque - y del OBJ) en 8 bits: en el bloque que empieza antes del OBJ da un valor
		 * grande (255 si empieza una fila antes) y no se acota: las filas salen de la VRAM que sigue (o precede, con
		 * vflip) a las teselas del OBJ, como en melonDS 1.1 */
		dy = (dy - line % mv) & 255;
	}
	const int dx = x - o->x;

	int sx, sy;
	if (o->aff) {
		const int cx = o->bw / 2, cy = o->bh / 2;

		sx = ((o->pa * (dx - cx) + o->pb * (dy - cy)) >> 8) + o->w / 2;
		sy = ((o->pc * (dx - cx) + o->pd * (dy - cy)) >> 8) + o->h / 2;
		if (sx < 0 || sx >= o->w || sy < 0 || sy >= o->h)
			return r;
	} else {
		sx = o->flipx ? o->w - 1 - dx : dx;
		sy = o->flipy ? o->h - 1 - dy : dy;
	}
	r.inbox = 1;
	if (o->mode == 3) {  /* mapa de bits */
		uint32_t addr;

		if (dc & G2D_DC_OBJBMP_1D) {
			addr = (uint32_t)o->tile * ((dc & G2D_DC_OBJBMP_BND) && !s->isb ? 256u : 128u) +
			       (uint32_t)(sy * o->w + sx) * 2u;
		} else {
			const uint32_t maskx = (dc & G2D_DC_OBJBMP_256) ? 0x1Fu : 0x0Fu;
			const uint32_t stride = (maskx + 1) * 16u;

			addr = ((uint32_t)o->tile & maskx) * 16u + ((uint32_t)o->tile & ~maskx) * 128u +
			       (uint32_t)sy * stride + (uint32_t)sx * 2u;
		}
		const uint16_t v = obj16(e, addr);

		if (!(v & 0x8000u))
			return r;
		r.v = 1;
		r.c = v & 0x7FFFu;
		r.balpha = o->alpha;
		return r;
	}
	const int tx = sx >> 3, ty = sy >> 3, ix = sx & 7, iy = sy & 7, wt = o->w >> 3;
	const uint32_t tsz = o->c256 ? 64u : 32u;
	uint32_t addr;

	if (dc & G2D_DC_OBJ_1D)
		addr = ((uint32_t)o->tile << (5 + G2D_DC_OBJ_SHIFT(dc))) + (uint32_t)(ty * wt + tx) * tsz;
	else
		addr = ((uint32_t)o->tile + (uint32_t)ty * 32u + (uint32_t)tx * (o->c256 ? 2u : 1u)) * 32u;
	int c;
	if (o->c256) {
		c = obj8(e, addr + (uint32_t)(iy * 8 + ix));
	} else {
		const uint8_t byte = obj8(e, addr + (uint32_t)(iy * 4 + (ix >> 1)));

		c = (ix & 1) ? byte >> 4 : byte & 15;
	}
	if (!c)
		return r;
	r.v = 1;
	if (o->mode == 2) {
		r.win = 1;
		return r;
	}
	if (o->c256)
		r.c = (dc & G2D_DC_OBJEXTPAL) ? ext_color(e, e->extobj, (uint32_t)o->pal * 256u + (uint32_t)c) : s->pal[256 + c];
	else
		r.c = s->pal[256 + o->pal * 16 + c];
	r.semi = o->mode == 1;
	return r;
}

/* ---- ventanas ------------------------------------------------------------------------------------- */

static int win_inside_h(uint16_t reg, int x)
{
	const int x1 = reg >> 8;
	int x2 = reg & 255;

	if (x1 == 0 && x2 == 0)
		return 0;
	if (x2 == 0)
		x2 = 256;
	if (x1 <= x2)
		return x >= x1 && x < x2;
	return x >= x1 || x < x2;
}

/* Avanza la bandera vertical de la ventana `i` por una línea (0..262; solo cuentan los 8 bits bajos). */
static void win_step_v(rs_t *s, int i, int line)
{
	const uint16_t reg = s->regs[G2D_R_WIN0V + i];
	const int y1 = reg >> 8, y2 = reg & 255;

	if ((line & 255) == y2)
		s->winv[i] = 0;
	if ((line & 255) == y1)
		s->winv[i] = 1;
}

/* ---- color ---------------------------------------------------------------------------------------- */

/* ---- una línea -------------------------------------------------------------------------------------- */

static void render_line(rs_t *s, int y, uint16_t *out)
{
	const gpu2d_engine *e = s->e;
	const uint32_t dc = s->dc;
	const int mode = s->isb ? (int)(G2D_DC_DISPMODE(dc) & 1) : (int)G2D_DC_DISPMODE(dc);
	const uint16_t mb = s->regs[G2D_R_MASTER_BRIGHT];

	if ((dc & G2D_DC_BLANK) || mode == 0) {
		for (int x = 0; x < GPU2D_W; x++)
			out[x] = gpu2d_to_5551(g2d_master_bright(0x7FFF, mb));
		return;
	}
	if (mode == 2) {
		const uint32_t base = e->vdisp[G2D_DC_VRAMBANK(dc)];

		for (int x = 0; x < GPU2D_W; x++) {
			const uint16_t c = base == G2D_UNMAPPED
						   ? 0
						   : (uint16_t)(rd16le(vram_of(e) + base + (uint32_t)(y * GPU2D_W + x) * 2u) & 0x7FFF);

			out[x] = gpu2d_to_5551(g2d_master_bright(c, mb));
		}
		return;
	}
	if (mode == 3) {
		const uint8_t *m = (const uint8_t *)(uintptr_t)e->mmem_ea;

		for (int x = 0; x < GPU2D_W; x++) {
			const uint16_t c = m ? (uint16_t)(rd16le(m + y * 512 + x * 2) & 0x7FFF) : 0;

			out[x] = gpu2d_to_5551(g2d_master_bright(c, mb));
		}
		return;
	}

	/* modo 1: gráficos */
	robj objs[GPU2D_NOBJ];
	opx_t oline[GPU2D_W];
	int owin[GPU2D_W];
	int nobj = 0;
	int bgt[GPU2D_NBG];
	const int use_win = (dc & (G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN)) != 0;
	const uint16_t bld = s->regs[G2D_R_BLDCNT], balpha = s->regs[G2D_R_BLDALPHA];
	const int t1 = bld & 63, t2 = (bld >> 8) & 63, emode = (bld >> 6) & 3;
	const int eva = g2d_ev(balpha & 31), evb = g2d_ev((balpha >> 8) & 31), evy = g2d_ev(s->regs[G2D_R_BLDY] & 31);

	for (int b = 0; b < GPU2D_NBG; b++)
		bgt[b] = bg_type(s, b);
	if (dc & G2D_DC_OBJ) {
		for (int i = 0; i < GPU2D_NOBJ; i++) {
			robj o;

			if (!decode_obj(s, i, &o))
				continue;
			if (((y - o.y) & 255) >= o.bh)
				continue;
			objs[nobj++] = o;
		}
	}
	/* Línea de OBJ: por píxel, el OBJ de mejor prioridad (a igualdad, el de menor índice); los de ventana aparte. Después,
	 * el mosaico horizontal sobre esa línea (melonDS 1.1, programas gfxmos*): un píxel de un OBJ con mosaico toma el del
	 * principio de su bloque de pantalla, y donde empieza una racha de píxeles con mosaico (después de otro tipo de
	 * píxel) el primero hace de principio de bloque. */
	{
		const int mh = ((s->regs[G2D_R_MOSAIC] >> 8) & 15) + 1;
		opx_t latch = { 0, 0, 0, -1, 0, 0, 0, 0 };
		int prev_flag = 0;

		for (int x = 0; x < GPU2D_W; x++) {
			opx_t best = { 0, 0, 0, -1, 0, 0, 0, 0 };
			int bp = 99, ow = 0, cover = 0;

			for (int k = 0; k < nobj; k++) {
				const robj *o = &objs[k];
				opx_t t = obj_pixel(s, o, x, y);

				if (!t.v) {
					if (t.inbox && o->mos && o->mode != 2)
						cover = 1;
					continue;
				}
				if (t.win) {
					ow = 1;
					continue;
				}
				t.prio = o->prio;
				t.mos = o->mos;
				if (o->prio < bp) {
					bp = o->prio;
					best = t;
				}
			}
			if (!best.v && cover) { /* texel transparente de un OBJ con mosaico: sin color pero con la marca */
				best.mos = 1;
				best.inbox = 1;
			}
			oline[x] = best;
			owin[x] = ow;
		}
		for (int x = 0; x < GPU2D_W; x++) {
			const int flag = oline[x].mos;

			if (flag) {
				if (x % mh == 0 || !prev_flag)
					latch = oline[x];
				else
					oline[x] = latch;
			}
			prev_flag = flag;
		}
	}
	for (int x = 0; x < GPU2D_W; x++) {
		/* OBJ: el de mejor prioridad (a igualdad, el de menor índice); los de ventana aparte */
		opx_t op = oline[x];
		int best_prio = op.v ? op.prio : 99, objwin = owin[x];

		/* ventanas */
		int wbits = 0x3F;

		if (use_win) {
			const int in0 = (dc & G2D_DC_WIN0) && s->winv[0] && win_inside_h(s->regs[G2D_R_WIN0H], x);
			const int in1 = (dc & G2D_DC_WIN1) && s->winv[1] && win_inside_h(s->regs[G2D_R_WIN1H], x);

			if (in0)
				wbits = s->regs[G2D_R_WININ] & 0x3F;
			else if (in1)
				wbits = (s->regs[G2D_R_WININ] >> 8) & 0x3F;
			else if ((dc & G2D_DC_OBJWIN) && objwin)
				wbits = (s->regs[G2D_R_WINOUT] >> 8) & 0x3F;
			else
				wbits = s->regs[G2D_R_WINOUT] & 0x3F;
		}
		const int effects_ok = !use_win || (wbits & 0x20);
		/* orden de capas de adelante hacia atrás */
		int lay[6], nl = 0;
		uint16_t col[6];
		int semi = 0, balp = -1, a3top = -1, l3top = 0;

		for (int p = 0; p < 4; p++) {
			if (op.v && best_prio == p && (!use_win || (wbits & 0x10))) {
				lay[nl] = 4;
				col[nl++] = op.c;
				semi = op.semi;
				balp = op.balpha;
			}
			for (int b = 0; b < GPU2D_NBG; b++) {
				if (bgt[b] == BG_OFF || (int)G2D_BGCNT_PRIO(s->regs[G2D_R_BGCNT + b]) != p)
					continue;
				if (use_win && !(wbits & (1 << b)))
					continue;
				if (bgt[b] == BG_3D) {
					/* capa 3D (Fase 3.2): el píxel del motor de render, desplazado por BG0HOFS (9 bits) */
					const uint32_t *img = (const uint32_t *)(uintptr_t)s->e->img3d_ea;
					const int xs = (x + (s->regs[G2D_R_BGHOFS] & 0x1FF)) & 0x1FF;
					uint32_t px, a;

					if (!img || xs >= GPU2D_W)
						continue;
					px = img[y * GPU2D_W + xs];
					a = (px >> 24) & 31u;
					if (a == 0)
						continue;
					if (nl == 0) {
						a3top = (int)a;
						l3top = g2d_3d_lsb(px);
					}
					lay[nl] = 0;
					col[nl++] = g2d_3d_555(px);
					continue;
				}
				const px_t t = bgt[b] == BG_TEXT ? bg_text(s, b, x, y) : bg_affine(s, bgt[b], b, x, y);

				if (t.v) {
					lay[nl] = b;
					col[nl++] = t.c;
				}
			}
		}
		lay[nl] = 5;  /* color de fondo */
		col[nl++] = s->pal[0];
		const int top_l = lay[0];
		uint16_t c = col[0];
		const int sec_l = nl > 1 ? lay[1] : 5;
		const uint16_t sec_c = nl > 1 ? col[1] : s->pal[0];

		if (top_l == 0 && a3top >= 0) {
			/* 3D (también el opaco): con el objetivo 2 se mezcla con su propio alfa, sin mirar el modo de BLDCNT ni EVA/EVB
			 * (el opaco queda igual); sin objetivo 2, como un OBJ semitransparente (brillo si BG0 es objetivo 1) [melonDS] */
			if (t2 & (1 << sec_l))
				c = g2d_blend3d(c, sec_c, a3top, l3top);
			else if (effects_ok && (t1 & 1) && emode >= 2)
				c = emode == 2 ? g2d_bright_up(c, evy) : g2d_bright_down(c, evy);
		} else if (top_l == 4 && (semi || balp >= 0)) {
			if (t2 & (1 << sec_l)) {
				const int a = balp >= 0 ? balp + 1 : eva, b = balp >= 0 ? 16 - a : evb;

				c = g2d_blend(c, sec_c, a, b);
			} else if (effects_ok && (t1 & 0x10) && emode >= 2) {
				c = emode == 2 ? g2d_bright_up(c, evy) : g2d_bright_down(c, evy);
			}
		} else if (effects_ok && emode && (t1 & (1 << top_l))) {
			if (emode == 1) {
				if (t2 & (1 << sec_l))
					c = g2d_blend(c, sec_c, eva, evb);
			} else {
				c = emode == 2 ? g2d_bright_up(c, evy) : g2d_bright_down(c, evy);
			}
		}
		out[x] = gpu2d_to_5551(g2d_master_bright(c, mb));
	}
}

void gpu2d_ref_engine(const gpu2d_engine *e, uint16_t *out)
{
	rs_t s;
	const gpu2d_change *chg = (const gpu2d_change *)(uintptr_t)e->chg_ea;
	const uint8_t *pal = (const uint8_t *)(uintptr_t)e->pal_ea, *oam = (const uint8_t *)(uintptr_t)e->oam_ea;
	uint32_t next = 0;

	memset(&s, 0, sizeof s);
	s.e = e;
	s.isb = (e->flags & G2D_F_ENGINE_B) != 0;
	memcpy(s.regs, e->regs, sizeof s.regs);
	for (int i = 0; i < 512; i++)
		s.pal[i] = (uint16_t)(rd16le(pal + i * 2) & 0x7FFFu);
	memcpy(s.oam, oam, 1024);
	reload_ref(&s, 0);
	reload_ref(&s, 1);

	/* Las ventanas verticales: la bandera sigue corriendo por las 263 líneas (solo cuentan los 8 bits bajos de la
	 * línea), así que su valor al empezar el cuadro sale de dar la vuelta al cuadro anterior con los mismos
	 * registros. */
	for (int pass = 0; pass < 2; pass++)
		for (int line = 0; line < 263; line++) {
			win_step_v(&s, 0, line);
			win_step_v(&s, 1, line);
		}

	for (int y = 0; y < GPU2D_H; y++) {
		while (next < e->nchg && chg[next].line <= y)
			apply_change(&s, &chg[next++]);
		s.dc = (uint32_t)s.regs[G2D_R_DISPCNT] | ((uint32_t)s.regs[G2D_R_DISPCNT + 1] << 16);
		if (s.isb)
			s.dc &= ~(G2D_DC_3D | G2D_DC_OBJBMP_BND | 0x000C0000u | 0x3F000000u);
		/* mosaico vertical: al empezar un grupo se fija el punto de referencia */
		if (y % (((s.regs[G2D_R_MOSAIC] >> 4) & 15) + 1) == 0)
			memcpy(s.mref, s.ref, sizeof s.mref);
		win_step_v(&s, 0, y);
		win_step_v(&s, 1, y);
		render_line(&s, y, out + y * GPU2D_W);
		/* al final de la línea: los puntos de referencia avanzan con PB y PD */
		for (int b = 0; b < 2; b++) {
			const int pbase = b ? G2D_R_BG3PA : G2D_R_BG2PA;

			s.ref[b][0] += (int16_t)s.regs[pbase + 1];
			s.ref[b][1] += (int16_t)s.regs[pbase + 3];
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
