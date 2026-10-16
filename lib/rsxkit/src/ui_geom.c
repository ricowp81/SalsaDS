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

_Static_assert(COLS * CELL_W == RSXKIT_UI_ATLAS_W, "ancho del atlas");

static struct {
	rsxkit_vertex *v;
	uint32_t n, cap;
	float lw, lh;
	uint32_t dropped;
	float white_s, white_t;  /* centro del bloque blanco */
	uint32_t group[RSXKIT_UI_MAX_GROUPS], ngroups;
	uint8_t group_img[RSXKIT_UI_MAX_GROUPS];  /* 0: atlas; n: ranura n − 1 */
	uint32_t img_w[RSXKIT_UI_IMAGES], img_h[RSXKIT_UI_IMAGES];
} b;

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

void rsxkit_ui_atlas(uint8_t *rgba, uint32_t pitch)
{
	int total = rsxkit_glyph_total();
	_Static_assert(RSXKIT_UI_ATLAS_H % CELL_H == 0, "alto del atlas");
	for (uint32_t y = 0; y < RSXKIT_UI_ATLAS_H; y++) {
		uint8_t *p = rgba + (size_t)y * pitch;
		for (uint32_t x = 0; x < RSXKIT_UI_ATLAS_W; x++, p += 4)
			p[0] = p[1] = p[2] = 0xFF, p[3] = 0;
	}
	for (int g = 0; g <= total; g++) {
		uint32_t ox, oy;
		cell_origin(g, &ox, &oy);
		if (oy + GH >= RSXKIT_UI_ATLAS_H)
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
	b.lw = lw;
	b.lh = lh;
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
	if (slot < 0 || slot >= RSXKIT_UI_IMAGES || !b.img_w[slot] || !b.img_h[slot] || w <= 0.0f || h <= 0.0f ||
	    !(rgba & 0xFF))
		return;
	if (b.ngroups + 1 >= RSXKIT_UI_MAX_GROUPS) {  /* el último grupo no puede quedar de imagen */
		b.dropped++;
		return;
	}
	const float iw = (float)b.img_w[slot], ih = (float)b.img_h[slot];
	const float p[4][2] = { { x, y }, { x + w, y }, { x, y + h }, { x + w, y + h } };
	const float st[4][2] = { { 0.0f, 0.0f }, { iw, 0.0f }, { 0.0f, ih }, { iw, ih } };
	const uint32_t c[4] = { rgba, rgba, rgba, rgba };
	new_group();
	b.group_img[b.ngroups - 1] = (uint8_t)(slot + 1);
	quad(p, st, c);  /* lo que siga abre su propio grupo (new_group) */
}

float rsxkit_ui_text(float x, float y, const char *utf8, float scale, uint32_t rgba, uint32_t max_cells)
{
	if (!utf8)
		return 0.0f;
	float w = RSXKIT_FONT_W * scale, h = RSXKIT_FONT_H * scale;
	uint32_t total = max_cells ? rsxkit_text_cells(utf8) : 0, n = 0;
	const uint32_t c[4] = { rgba, rgba, rgba, rgba };
	new_group();
	while (*utf8) {
		uint32_t cp = rsxkit_utf8_next(&utf8);
		if (max_cells && total > max_cells && n == max_cells - 1)
			cp = 0x2026;  /* «…» */
		if (cp != ' ' && (rgba & 0xFF)) {
			uint32_t ox, oy;
			cell_origin(rsxkit_glyph_index(cp), &ox, &oy);
			float x0 = x + n * w;
			const float p[4][2] = { { x0, y }, { x0 + w, y }, { x0, y + h }, { x0 + w, y + h } };
			const float st[4][2] = { { (float)ox, (float)oy }, { (float)(ox + GW), (float)oy },
						 { (float)ox, (float)(oy + GH) }, { (float)(ox + GW), (float)(oy + GH) } };
			quad(p, st, c);
		}
		if (++n == max_cells)
			break;
	}
	return n * w;
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
