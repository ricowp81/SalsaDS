/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — geometría del 2D de rsxkit/ui.h: el atlas de la fuente y los
 * triángulos de rectángulos, texto y ondas. Sin RSX ni PSL1GHT (compila en
 * cualquier host); ui.c lo envía al RSX.
 *
 * Atlas: cada glifo de 8 × 13 se agranda a 16 × 26 con Scale2x (la regla
 * de EPX de Andrea Mazzoleni, escrita aquí a partir de su descripción
 * pública): las diagonales salen suaves en vez de en escalera, y con el
 * filtro lineal del RSX el texto escala bien. Cada glifo va en una celda de
 * 18 × 28 con un borde transparente para que el filtro no mezcle vecinos.
 *
 * Caché de glifos de la fuente externa (rsxkit_ui_font): casillas de 28 × 28
 * en el resto del atlas, cada una con un glifo de hasta 26 × 26 y el mismo
 * borde. Una tabla de glifos conocidos (punto de código → avance, imagen y
 * casilla) guarda las medidas aunque la imagen salga del atlas, así que medir
 * rasteriza cada glifo una sola vez. Cuando no hay casilla libre se reemplaza
 * la usada hace más cuadros (LRU por cuadro), pero nunca una usada en este
 * cuadro o en el anterior: el RSX puede estar leyéndola todavía (ui.c espera
 * la barrera del cuadro de hace dos antes de armar el siguiente), así que no
 * hace falta esperar al RSX ni cortar el cuadro. Lo que no cabe se dibuja en
 * el cuadro siguiente.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "rsxkit/text.h"
#include "rsxkit/ui.h"

#include "font.h"

#define CELL_W 18
#define CELL_H 28
#define COLS   16
#define GW     (2 * RSXKIT_FONT_W)
#define GH     (2 * RSXKIT_FONT_H)
#define WAVE_SEGMENTS 64
#define TAU 6.28318530718f
#define ELLIPSIS 0x2026u

#define SLOT        RSXKIT_UI_GLYPH_SLOT
#define BOX         RSXKIT_GLYPH_BOX
#define RIGHT_COLS  ((RSXKIT_UI_ATLAS_W - RSXKIT_UI_ATLAS_FIXED_W) / SLOT)
#define RIGHT_ROWS  (RSXKIT_UI_ATLAS_FIXED_H / SLOT)
#define BELOW_COLS  (RSXKIT_UI_ATLAS_W / SLOT)
#define BELOW_ROWS  ((RSXKIT_UI_ATLAS_H - RSXKIT_UI_ATLAS_FIXED_H) / SLOT)
#define NSLOTS      RSXKIT_UI_GLYPH_SLOTS
#define REC_CAP     2048   /* glifos conocidos (potencia de 2) */
#define REC_MAX     (REC_CAP * 3 / 4)
#define RENDER_BUDGET 32   /* glifos que vuelven al atlas por cuadro */

_Static_assert(COLS * CELL_W == RSXKIT_UI_ATLAS_FIXED_W, "ancho de la 8x13 en el atlas");
_Static_assert(RSXKIT_UI_ATLAS_FIXED_H % CELL_H == 0, "alto de la 8x13 en el atlas");
_Static_assert(SLOT == BOX + 2, "casilla = glifo y un borde transparente");
_Static_assert(RIGHT_COLS * RIGHT_ROWS + BELOW_COLS * BELOW_ROWS == NSLOTS, "casillas del atlas");
_Static_assert((REC_CAP & (REC_CAP - 1)) == 0, "tabla de glifos en potencia de 2");

static struct {
	rsxkit_vertex *v;
	uint32_t n, cap;
	float lw, lh;
	uint32_t dropped;
	float white_s, white_t;  /* centro del bloque blanco */
	uint32_t group[RSXKIT_UI_MAX_GROUPS], ngroups;
	uint8_t group_img[RSXKIT_UI_MAX_GROUPS];  /* 0: atlas; n: ranura n − 1; HOOK_GROUP: un gancho */
	uint8_t group_hook[RSXKIT_UI_MAX_GROUPS]; /* con HOOK_GROUP, cuál de hooks[] */
	struct { rsxkit_ui_hook_fn fn; void *user; } hooks[RSXKIT_UI_MAX_HOOKS];
	uint32_t nhooks;
	uint32_t img_w[RSXKIT_UI_IMAGES], img_h[RSXKIT_UI_IMAGES];
} b;

/* Un glifo conocido de la fuente externa. */
enum { REC_EMPTY, REC_OK, REC_MISSING };
struct rec {
	uint32_t cp;
	float adv;      /* unidades lógicas a escala 1 */
	int8_t left;    /* téxeles del atlas */
	uint8_t width;  /* téxeles del atlas */
	uint8_t state;
	int16_t slot;   /* casilla del atlas, o −1 */
};

static struct {
	const rsxkit_glyph_source *src;
	uint8_t *px;            /* el atlas (rsxkit_ui_atlas) */
	uint32_t pitch;
	uint32_t frame;         /* lotes empezados */
	uint32_t budget;        /* rasterizados que quedan en este cuadro para volver al atlas */
	int dirty;
	uint32_t nrec;
	struct rec rec[REC_CAP];
	struct {
		uint32_t cp;     /* 0: libre */
		uint32_t stamp;  /* cuadro del último uso + 1 (0: nunca) */
	} slot[NSLOTS];
	rsxkit_ui_font_stats st;
} fc;

/* Lo que sigue es otro grupo (otra llamada al RSX). */
static void new_group(void)
{
	if (!(b.ngroups && b.group[b.ngroups - 1] == b.n) && b.ngroups < RSXKIT_UI_MAX_GROUPS)
		b.group[b.ngroups++] = b.n;
	if (b.ngroups)
		b.group_img[b.ngroups - 1] = 0;  /* un grupo reusado vuelve a ser del atlas */
}

static void cell_origin(int index, uint32_t *x, uint32_t *y)
{
	*x = (uint32_t)(index % COLS) * CELL_W + 1;
	*y = (uint32_t)(index / COLS) * CELL_H + 1;
}

static int bit(const uint8_t *rows, int x, int y)
{
	if (x < 0 || y < 0 || x >= RSXKIT_FONT_W || y >= RSXKIT_FONT_H)
		return 0;
	return (rows[y] >> (7 - x)) & 1;
}

static void cache_reset(void);

void rsxkit_ui_atlas(uint8_t *rgba, uint32_t pitch)
{
	int total = rsxkit_glyph_total();
	fc.px = rgba;
	fc.pitch = pitch;
	cache_reset();
	for (uint32_t y = 0; y < RSXKIT_UI_ATLAS_H; y++) {
		uint8_t *p = rgba + (size_t)y * pitch;
		for (uint32_t x = 0; x < RSXKIT_UI_ATLAS_W; x++, p += 4)
			p[0] = p[1] = p[2] = 0xFF, p[3] = 0;
	}
	for (int g = 0; g <= total; g++) {
		uint32_t ox, oy;
		cell_origin(g, &ox, &oy);
		if (oy + GH >= RSXKIT_UI_ATLAS_FIXED_H)
			break;
		const uint8_t *rows = g < total ? rsxkit_glyph_rows(g) : NULL;
		for (int y = 0; y < RSXKIT_FONT_H; y++)
			for (int x = 0; x < RSXKIT_FONT_W; x++) {
				int e[4];
				if (!rows) {
					e[0] = e[1] = e[2] = e[3] = 1;  /* el bloque blanco */
				} else {
					/* Scale2x: P y sus vecinos A arriba, B derecha, C
					 * izquierda, D abajo. */
					int P = bit(rows, x, y), A = bit(rows, x, y - 1), B = bit(rows, x + 1, y);
					int C = bit(rows, x - 1, y), D = bit(rows, x, y + 1);
					e[0] = C == A && C != D && A != B ? A : P;
					e[1] = A == B && A != C && B != D ? B : P;
					e[2] = D == C && D != B && C != A ? C : P;
					e[3] = B == D && B != A && D != C ? D : P;
				}
				for (int k = 0; k < 4; k++) {
					uint32_t px = ox + 2 * (uint32_t)x + (k & 1), py = oy + 2 * (uint32_t)y + (k >> 1);
					rgba[(size_t)py * pitch + 4 * px + 3] = e[k] ? 0xFF : 0;
				}
			}
	}
}

void rsxkit_ui_batch(rsxkit_vertex *v, uint32_t cap, float lw, float lh)
{
	b.v = v;
	b.cap = v ? cap : 0;
	b.n = 0;
	b.group[0] = 0;
	b.group_img[0] = 0;
	b.ngroups = 1;
	b.nhooks = 0;
	b.lw = lw;
	b.lh = lh;
	if (v) {
		fc.frame++;
		fc.budget = RENDER_BUDGET;
	}
	uint32_t ox, oy;
	cell_origin(rsxkit_glyph_total(), &ox, &oy);
	b.white_s = ox + GW * 0.5f;
	b.white_t = oy + GH * 0.5f;
}

uint32_t rsxkit_ui_count(void)
{
	return b.n;
}

uint32_t rsxkit_ui_groups(const uint32_t **first)
{
	/* Un grupo vacío al final (nada después de la última marca) no cuenta. */
	uint32_t n = b.ngroups;
	if (n && b.group[n - 1] == b.n)
		n--;
	*first = b.group;
	return n;
}

uint32_t rsxkit_ui_dropped(void)
{
	return b.dropped;
}

void rsxkit_ui_image_size(int slot, uint32_t w, uint32_t h)
{
	if (slot >= 0 && slot < RSXKIT_UI_IMAGES) {
		b.img_w[slot] = w;
		b.img_h[slot] = h;
	}
}

uint32_t rsxkit_ui_group_image(uint32_t g)
{
	return g < b.ngroups ? b.group_img[g] : 0;
}

void rsxkit_ui_hook(rsxkit_ui_hook_fn fn, void *user)
{
	/* Con un grupo más después, para que lo siguiente no herede el gancho. */
	if (!fn || !b.v || b.nhooks >= RSXKIT_UI_MAX_HOOKS || b.ngroups + 2 >= RSXKIT_UI_MAX_GROUPS) {
		b.dropped++;
		return;
	}
	new_group();
	b.group_img[b.ngroups - 1] = RSXKIT_UI_HOOK_GROUP;
	b.group_hook[b.ngroups - 1] = (uint8_t)b.nhooks;
	b.hooks[b.nhooks].fn = fn;
	b.hooks[b.nhooks].user = user;
	b.nhooks++;
	/* Cierra el grupo del gancho: el siguiente dibujo abre otro. */
	if (b.ngroups < RSXKIT_UI_MAX_GROUPS)
		b.group[b.ngroups++] = b.n;
	b.group_img[b.ngroups - 1] = 0;
}

void rsxkit_ui_group_hook(uint32_t g, rsxkit_ui_hook_fn *fn, void **user)
{
	if (g < b.ngroups && b.group_img[g] == RSXKIT_UI_HOOK_GROUP) {
		*fn = b.hooks[b.group_hook[g]].fn;
		*user = b.hooks[b.group_hook[g]].user;
	} else {
		*fn = NULL;
		*user = NULL;
	}
}

static void vertex(rsxkit_vertex *v, float x, float y, float s, float t, uint32_t rgba)
{
	v->pos[0] = x;
	v->pos[1] = y;
	v->pos[2] = 0.0f;
	v->pos[3] = 1.0f;
	v->st[0] = s;
	v->st[1] = t;
	v->rgba[0] = (uint8_t)(rgba >> 24);
	v->rgba[1] = (uint8_t)(rgba >> 16);
	v->rgba[2] = (uint8_t)(rgba >> 8);
	v->rgba[3] = (uint8_t)rgba;
	v->pad = 0;
}

/* Cuadrilátero p0 p1 (arriba) p2 p3 (abajo), dos triángulos. Las
 * coordenadas de textura van por esquina en st[4][2]. */
static void quad(const float p[4][2], const float st[4][2], const uint32_t c[4])
{
	if (b.n + 6 > b.cap) {
		b.dropped++;
		return;
	}
	static const int order[6] = { 0, 1, 2, 1, 3, 2 };
	rsxkit_vertex *v = b.v + b.n;
	for (int i = 0; i < 6; i++) {
		int k = order[i];
		vertex(&v[i], p[k][0], p[k][1], st[k][0], st[k][1], c[k]);
	}
	b.n += 6;
}

void rsxkit_ui_gradient(float x, float y, float w, float h, uint32_t tl, uint32_t tr, uint32_t bl, uint32_t br)
{
	if (w <= 0.0f || h <= 0.0f || !((tl | tr | bl | br) & 0xFF))
		return;
	const float p[4][2] = { { x, y }, { x + w, y }, { x, y + h }, { x + w, y + h } };
	const float st[4][2] = { { b.white_s, b.white_t }, { b.white_s, b.white_t }, { b.white_s, b.white_t },
				 { b.white_s, b.white_t } };
	const uint32_t c[4] = { tl, tr, bl, br };
	new_group();
	quad(p, st, c);
}

void rsxkit_ui_image(int slot, float x, float y, float w, float h, uint32_t rgba)
{
	if (slot < 0 || slot >= RSXKIT_UI_IMAGES)
		return;
	rsxkit_ui_image_part(slot, x, y, w, h, 0.0f, 0.0f, (float)b.img_w[slot], (float)b.img_h[slot], rgba);
}

void rsxkit_ui_image_part(int slot, float x, float y, float w, float h, float sx, float sy, float sw, float sh,
			  uint32_t rgba)
{
	if (slot < 0 || slot >= RSXKIT_UI_IMAGES || !b.img_w[slot] || !b.img_h[slot] || w <= 0.0f || h <= 0.0f ||
	    sw <= 0.0f || sh <= 0.0f || !(rgba & 0xFF))
		return;
	if (b.ngroups + 1 >= RSXKIT_UI_MAX_GROUPS) {  /* el último grupo no puede quedar de imagen */
		b.dropped++;
		return;
	}
	const float p[4][2] = { { x, y }, { x + w, y }, { x, y + h }, { x + w, y + h } };
	const float st[4][2] = { { sx, sy }, { sx + sw, sy }, { sx, sy + sh }, { sx + sw, sy + sh } };
	const uint32_t c[4] = { rgba, rgba, rgba, rgba };
	new_group();
	b.group_img[b.ngroups - 1] = (uint8_t)(slot + 1);
	quad(p, st, c);  /* lo que siga abre su propio grupo (new_group) */
}

/* ---- Caché de glifos ----------------------------------------------------- */

/* Casilla `i`: origen de la imagen (dentro del borde). */
static void slot_origin(int i, uint32_t *x, uint32_t *y)
{
	if (i < RIGHT_COLS * RIGHT_ROWS) {
		*x = RSXKIT_UI_ATLAS_FIXED_W + (uint32_t)(i % RIGHT_COLS) * SLOT + 1;
		*y = (uint32_t)(i / RIGHT_COLS) * SLOT + 1;
	} else {
		i -= RIGHT_COLS * RIGHT_ROWS;
		*x = (uint32_t)(i % BELOW_COLS) * SLOT + 1;
		*y = RSXKIT_UI_ATLAS_FIXED_H + (uint32_t)(i / BELOW_COLS) * SLOT + 1;
	}
}

/* Olvida los glifos conocidos y las casillas (las marcas de uso quedan: una
 * casilla que el RSX puede estar leyendo no se reusa todavía). */
static void cache_reset(void)
{
	memset(fc.rec, 0, sizeof fc.rec);
	fc.nrec = 0;
	for (int i = 0; i < NSLOTS; i++)
		fc.slot[i].cp = 0;
}

void rsxkit_ui_font(const rsxkit_glyph_source *src)
{
	if (src == fc.src)
		return;
	fc.src = src;
	cache_reset();
	fc.st.resets++;
}

const rsxkit_glyph_source *rsxkit_ui_font_source(void)
{
	return fc.src;
}

void rsxkit_ui_font_stats_get(rsxkit_ui_font_stats *s)
{
	uint32_t resident = 0;
	for (int i = 0; i < NSLOTS; i++)
		resident += fc.slot[i].cp != 0;
	fc.st.known = fc.nrec;
	fc.st.resident = resident;
	fc.st.slots = NSLOTS;
	*s = fc.st;
}

int rsxkit_ui_atlas_dirty(void)
{
	int d = fc.dirty;
	fc.dirty = 0;
	return d;
}

static struct rec *rec_find(uint32_t cp, int insert)
{
	uint32_t h = (cp * 2654435761u) >> 21 & (REC_CAP - 1);
	for (;; h = (h + 1) & (REC_CAP - 1)) {
		struct rec *r = &fc.rec[h];
		if (r->state != REC_EMPTY && r->cp == cp)
			return r;
		if (r->state == REC_EMPTY) {
			if (!insert)
				return NULL;
			if (fc.nrec >= REC_MAX) {  /* llena: se empieza de nuevo */
				cache_reset();
				fc.st.resets++;
				return rec_find(cp, 1);
			}
			fc.nrec++;
			r->cp = cp;
			r->slot = -1;
			return r;
		}
	}
}

/* Una casilla para un glifo nuevo: libre o la menos usada, nunca una que el
 * RSX pueda estar leyendo (usada en este cuadro o en el anterior). −1 si no
 * hay. */
static int slot_take(void)
{
	int best = -1;
	uint32_t best_stamp = 0;
	for (int i = 0; i < NSLOTS; i++) {
		uint32_t st = fc.slot[i].stamp;
		if (st && st + 1 > fc.frame)
			continue;
		if (!fc.slot[i].cp) {
			best = i;
			break;
		}
		if (best < 0 || st < best_stamp)
			best = i, best_stamp = st;
	}
	if (best < 0)
		return -1;
	if (fc.slot[best].cp) {
		struct rec *old = rec_find(fc.slot[best].cp, 0);
		if (old && old->slot == best)
			old->slot = -1;
		fc.st.evictions++;
	}
	fc.slot[best].cp = 0;
	return best;
}

/* Rasteriza `cp` con la fuente externa: llena el registro y, si `store` y
 * hay casilla, deja la imagen en el atlas. */
static void rec_render(struct rec *r, uint32_t cp, int store)
{
	uint8_t box[BOX * BOX];
	rsxkit_glyph_info g = { 0.0f, 0, 0 };
	memset(box, 0, sizeof box);
	fc.st.renders++;
	if (fc.src->render(fc.src->user, cp, box, BOX, &g) < 0) {
		r->state = REC_MISSING;
		return;
	}
	int w = g.width < 0 ? 0 : g.width > BOX ? BOX : g.width;
	int left = g.left < -BOX ? -BOX : g.left > BOX ? BOX : g.left;
	float adv = g.advance;
	if (!(adv >= 0.0f && adv <= 4.0f * BOX))  /* también NaN */
		adv = (float)(left + w + 2);
	r->state = REC_OK;
	r->adv = 0.5f * adv;
	r->left = (int8_t)left;
	r->width = (uint8_t)w;
	if (!store || !w || !fc.px)
		return;
	int s = slot_take();
	if (s < 0)
		return;
	uint32_t ox, oy;
	slot_origin(s, &ox, &oy);
	for (int y = 0; y < BOX; y++) {
		uint8_t *row = fc.px + (size_t)(oy + (uint32_t)y) * fc.pitch + 4 * (size_t)ox;
		for (int x = 0; x < BOX; x++)
			row[4 * x + 3] = x < w ? box[y * BOX + x] : 0;
	}
	fc.slot[s].cp = cp;
	fc.slot[s].stamp = fc.frame + 1;
	r->slot = (int16_t)s;
	fc.dirty = 1;
}

/* Lo necesario para medir o dibujar un carácter. */
struct gl {
	float adv;           /* unidades a escala 1 */
	float left, width;   /* unidades, desde la pluma */
	uint32_t s, t;       /* esquina en el atlas */
	uint32_t tw;         /* ancho en el atlas (el alto es GH = BOX) */
	int ink;             /* 0: no se dibuja */
};

static void fixed_glyph(uint32_t cp, struct gl *g)
{
	g->adv = RSXKIT_FONT_W;
	g->left = 0.0f;
	g->width = RSXKIT_FONT_W;
	cell_origin(rsxkit_glyph_index(cp), &g->s, &g->t);
	g->tw = GW;
	g->ink = cp != ' ';
}

static void glyph_get(uint32_t cp, int draw, struct gl *g)
{
	if (!fc.src) {
		fixed_glyph(cp, g);
		return;
	}
	struct rec *r = rec_find(cp, 0);
	if (!r) {
		r = rec_find(cp, 1);
		rec_render(r, cp, 1);
	}
	if (r->state == REC_MISSING) {
		if (draw)
			fc.st.fallback++;
		fixed_glyph(cp, g);
		return;
	}
	g->adv = r->adv;
	g->left = 0.5f * r->left;
	g->width = 0.5f * r->width;
	g->tw = r->width;
	g->ink = r->width != 0;
	if (!draw || !g->ink)
		return;
	if (r->slot < 0) {
		if (fc.budget) {
			fc.budget--;
			rec_render(r, cp, 1);
		}
		if (r->slot < 0) {
			fc.st.deferred++;
			g->ink = 0;
			return;
		}
	} else {
		fc.st.hits++;
	}
	fc.slot[r->slot].stamp = fc.frame + 1;
	slot_origin(r->slot, &g->s, &g->t);
}

/* ---- Texto ------------------------------------------------------------- */

static float width_units(const char *utf8)
{
	float w = 0.0f;
	struct gl g;
	if (utf8)
		while (*utf8) {
			glyph_get(rsxkit_utf8_next(&utf8), 0, &g);
			w += g.adv;
		}
	return w;
}

float rsxkit_ui_text_width(const char *utf8, float scale)
{
	return width_units(utf8) * scale;
}

/* Arma (draw) o solo mide el texto con el límite max_w. */
static float run(float x, float y, const char *utf8, float scale, uint32_t rgba, float max_w, int draw)
{
	if (!utf8)
		return 0.0f;
	const float eps = 1e-3f;
	float h = RSXKIT_FONT_H * scale, ell = 0.0f;
	int cut = 0;
	struct gl g;
	if (max_w > 0.0f && width_units(utf8) * scale > max_w + eps) {
		cut = 1;
		glyph_get(ELLIPSIS, 0, &g);
		ell = g.adv;
	}
	const uint32_t c[4] = { rgba, rgba, rgba, rgba };
	if (draw)
		new_group();
	float pen = 0.0f;  /* unidades a escala 1 */
	while (*utf8) {
		uint32_t cp = rsxkit_utf8_next(&utf8);
		int last = 0;
		if (cut) {
			glyph_get(cp, 0, &g);
			if ((pen + g.adv + ell) * scale > max_w + eps)
				cp = ELLIPSIS, last = 1;
		}
		if (draw && (rgba & 0xFF)) {
			glyph_get(cp, 1, &g);
			if (g.ink) {
				float x0 = x + (pen + g.left) * scale, x1 = x0 + g.width * scale;
				const float p[4][2] = { { x0, y }, { x1, y }, { x0, y + h }, { x1, y + h } };
				const float st[4][2] = { { (float)g.s, (float)g.t }, { (float)(g.s + g.tw), (float)g.t },
							 { (float)g.s, (float)(g.t + GH) }, { (float)(g.s + g.tw), (float)(g.t + GH) } };
				quad(p, st, c);
			}
		} else {
			glyph_get(cp, 0, &g);
		}
		pen += g.adv;
		if (last)
			break;
	}
	return pen * scale;
}

float rsxkit_ui_text_fit(float x, float y, const char *utf8, float scale, uint32_t rgba, float max_w)
{
	return run(x, y, utf8, scale, rgba, max_w, 1);
}

float rsxkit_ui_text_fit_width(const char *utf8, float scale, float max_w)
{
	return run(0.0f, 0.0f, utf8, scale, 0, max_w, 0);
}

float rsxkit_ui_text(float x, float y, const char *utf8, float scale, uint32_t rgba, uint32_t max_cells)
{
	return rsxkit_ui_text_fit(x, y, utf8, scale, rgba, max_cells ? (float)(max_cells * RSXKIT_FONT_W) * scale : 0.0f);
}

/* Japonés, chino o coreano: se puede cortar antes o después de ellos. */
static int cjk(uint32_t cp)
{
	return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xAC00 && cp <= 0xD7AF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
	       (cp >= 0xFF00 && cp <= 0xFFEF);
}

/* Puntuación que no empieza renglón. */
static int no_start(uint32_t cp)
{
	static const uint32_t list[] = { 0x3001, 0x3002, 0xFF0C, 0xFF0E, 0x300D, 0x300F, 0xFF09, 0x3015, 0xFF01, 0xFF1F,
					 0x30FC, 0x30FB, 0xFF1A, 0xFF1B, 0x3009, 0x300B, 0x3011, 0x2026, ')', ',', '.',
					 ':', ';', '!', '?' };
	for (unsigned i = 0; i < sizeof list / sizeof list[0]; i++)
		if (cp == list[i])
			return 1;
	return 0;
}

size_t rsxkit_ui_text_wrap(const char *utf8, float scale, float max_w, const char **next)
{
	if (!utf8 || !*utf8) {
		if (next)
			*next = utf8;
		return 0;
	}
	const float eps = 1e-3f;
	const char *p = utf8, *brk = NULL, *brk_next = NULL;
	float pen = 0.0f;
	uint32_t prev = 0;
	struct gl g;
	while (*p) {
		const char *at = p;
		uint32_t cp = rsxkit_utf8_next(&p);
		if (cp == ' ') {
			/* Corte en el espacio: el renglón termina antes y el siguiente
			 * empieza después de todos los espacios. */
			const char *q = p;
			while (*q == ' ')
				q++;
			brk = at, brk_next = q;
		} else if (at != utf8 && (cjk(cp) || cjk(prev)) && !no_start(cp) && prev != ' ') {
			brk = at, brk_next = at;
		}
		glyph_get(cp, 0, &g);
		if (cp != ' ' && (pen + g.adv) * scale > max_w + eps && at != utf8) {
			if (!brk || brk == utf8) {  /* una palabra sola más ancha: entre dos caracteres */
				brk = at, brk_next = at;
			}
			/* Sin espacios al final del renglón. */
			const char *end = brk;
			while (end > utf8 && end[-1] == ' ')
				end--;
			if (end == utf8)
				end = brk, brk_next = brk;
			if (next)
				*next = brk_next;
			return (size_t)(end - utf8);
		}
		pen += g.adv;
		prev = cp;
	}
	if (next)
		*next = p;
	return (size_t)(p - utf8);
}

static float crest_at(const rsxkit_wave *w, float u, float t)
{
	return w->y + w->amp[0] * sinf(TAU * (w->freq[0] * u + w->speed[0] * t + w->phase)) +
	       w->amp[1] * sinf(TAU * (w->freq[1] * u + w->speed[1] * t + 2.0f * w->phase + 0.25f));
}

void rsxkit_ui_wave(const rsxkit_wave *w, float t)
{
	if (!w || b.lw <= 0.0f)
		return;
	uint32_t sheet0 = w->sheet & ~0xFFu, crest0 = w->crest & ~0xFFu;
	const float ws[4][2] = { { b.white_s, b.white_t }, { b.white_s, b.white_t }, { b.white_s, b.white_t },
				 { b.white_s, b.white_t } };
	float x[WAVE_SEGMENTS + 1], c[WAVE_SEGMENTS + 1], d[WAVE_SEGMENTS + 1];
	for (int i = 0; i <= WAVE_SEGMENTS; i++) {
		float u = (float)i / WAVE_SEGMENTS;
		x[i] = u * b.lw;
		c[i] = crest_at(w, u, t);
		d[i] = w->depth * (0.75f + 0.25f * sinf(TAU * (0.5f * u + w->phase - 0.5f * w->speed[0] * t)));
	}
	/* La lámina y, encima (en otro grupo), la línea de la cresta. */
	if (w->sheet & 0xFF) {
		new_group();
		for (int i = 0; i < WAVE_SEGMENTS; i++) {
			const float p[4][2] = { { x[i], c[i] }, { x[i + 1], c[i + 1] }, { x[i], c[i] + d[i] },
						{ x[i + 1], c[i + 1] + d[i + 1] } };
			const uint32_t col[4] = { w->sheet, w->sheet, sheet0, sheet0 };
			quad(p, ws, col);
		}
	}
	if (w->line > 0.0f && (w->crest & 0xFF)) {
		float l = w->line;
		new_group();
		for (int i = 0; i < WAVE_SEGMENTS; i++) {
			const float top[4][2] = { { x[i], c[i] - l }, { x[i + 1], c[i + 1] - l }, { x[i], c[i] },
						  { x[i + 1], c[i + 1] } };
			const float bot[4][2] = { { x[i], c[i] }, { x[i + 1], c[i + 1] }, { x[i], c[i] + l },
						  { x[i + 1], c[i + 1] + l } };
			const uint32_t ct[4] = { crest0, crest0, w->crest, w->crest };
			const uint32_t cb[4] = { w->crest, w->crest, crest0, crest0 };
			quad(top, ws, ct);
			quad(bot, ws, cb);
		}
	}
}
