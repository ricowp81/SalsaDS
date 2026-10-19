/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.4: rasterizador de referencia. Ver r3d_ref.h.
 */
#include "r3d_ref.h"

#include <math.h>
#include <stddef.h>

#include "r3d_core.h"

static uint32_t rd16le(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }

static const uint32_t WHITE = 64u | 64u << 8 | 64u << 16 | 32u << 24;

/* El texel (sx, ty) de la textura, decodificado como en la caché. */
static uint32_t ref_texel(const struct r3d_ref_in *in, uint32_t param, uint32_t pltt, uint32_t sx, uint32_t ty)
{
	const uint32_t w = R3D_TEX_SIZE_S(param), h = R3D_TEX_SIZE_T(param);
	const uint32_t bytes = r3d_tex_bytes(param), off = R3D_TEX_OFFSET(param);
	const uint32_t pb = r3d_pal_bytes(param), poff = (pltt & 0x1FFFu) * 16u;
	if (!bytes || w * h * 4u > R3D_TEX_PAGES * R3D_TEX_PAGE || off + bytes > in->vram_tex_size ||
	    poff + pb > in->vram_pal_size)
		return WHITE;
	const uint32_t i = ty * w + sx;
	uint32_t idx;
	switch (R3D_TEX_FORMAT(param)) {
	case R3D_FMT_DIRECT: {
		uint32_t c = rd16le(in->vram_tex + off + 2 * i);
		return r3d_texel(c & 0x7FFFu, c & 0x8000u ? 31u : 0u);
	}
	case R3D_FMT_PAL256:
		idx = in->vram_tex[off + i];
		break;
	default: /* R3D_FMT_PAL16 */
		idx = (in->vram_tex[off + i / 2] >> ((i & 1u) * 4u)) & 15u;
		break;
	}
	uint32_t c = rd16le(in->vram_pal + poff + 2 * idx);
	return r3d_texel(c, idx == 0 && (param & R3D_TEX_COLOR0) ? 0u : 31u);
}

static void ref_tri(const struct r3d_ref_in *in, const struct r3d_tri *t, uint32_t id, uint32_t *color,
		    uint32_t *depth)
{
	const int64_t sc = in->scale, W = R3D_W1 * sc, H = R3D_H1 * sc;
	int64_t X[3], Y[3];
	for (int k = 0; k < 3; k++) {
		X[k] = t->x[k] * sc;
		Y[k] = t->y[k] * sc;
	}
	int64_t area = (X[1] - X[0]) * (Y[2] - Y[0]) - (X[2] - X[0]) * (Y[1] - Y[0]);
	if (area == 0)
		return;
	int o[3] = { 0, 1, 2 };
	if (area < 0) {
		o[1] = 2;
		o[2] = 1;
		area = -area;
		int64_t tx = X[1], ty = Y[1];
		X[1] = X[2];
		Y[1] = Y[2];
		X[2] = tx;
		Y[2] = ty;
	}
	int64_t minx = X[0], maxx = X[0], miny = Y[0], maxy = Y[0];
	for (int k = 1; k < 3; k++) {
		minx = X[k] < minx ? X[k] : minx;
		maxx = X[k] > maxx ? X[k] : maxx;
		miny = Y[k] < miny ? Y[k] : miny;
		maxy = Y[k] > maxy ? Y[k] : maxy;
	}
	int64_t x0 = (minx + 7) >> 4, x1 = (maxx - 8) >> 4, y0 = (miny + 7) >> 4, y1 = (maxy - 8) >> 4;
	x0 = x0 < 0 ? 0 : x0;
	y0 = y0 < 0 ? 0 : y0;
	x1 = x1 > W - 1 ? W - 1 : x1;
	y1 = y1 > H - 1 ? H - 1 : y1;

	/* Borde k (de v[k+1] a v[k+2]) y si cumple la regla de arriba-izquierda. */
	int64_t dx[3], dy[3], ax[3], ay[3];
	int bias[3];
	for (int k = 0; k < 3; k++) {
		int a = (k + 1) % 3, b = (k + 2) % 3;
		dx[k] = X[b] - X[a];
		dy[k] = Y[b] - Y[a];
		ax[k] = X[a];
		ay[k] = Y[a];
		int64_t ea = -dy[k], eb = dx[k];
		bias[k] = !(ea > 0 || (ea == 0 && eb > 0));
	}

	const uint32_t param = t->texparam, w = R3D_TEX_SIZE_S(param), h = R3D_TEX_SIZE_T(param);
	const uint32_t lw = 3u + ((param >> 20) & 7u);
	const int rs = (param & R3D_TEX_REPEAT_S) != 0, rt = (param & R3D_TEX_REPEAT_T) != 0;
	const int textured = r3d_tex_bytes(param) != 0;
	double q[3], sq[3], tq[3], cq[3][3], z[3];
	for (int k = 0; k < 3; k++) {
		int v = o[k];
		q[k] = 4096.0 / (double)t->w[v];
		sq[k] = ((double)t->s[v] / 16.0 + 2048.0) * 4.0 * q[k];
		tq[k] = ((double)t->t[v] / 16.0 + 2048.0) * 4.0 * (double)w * q[k];
		z[k] = (double)t->z[v];
		uint32_t c = t->rgb[v];
		for (int ch = 0; ch < 3; ch++)
			cq[ch][k] = ((double)r3d_x6((c >> (5 * ch)) & 31u) + 1.5) * q[k];
	}
	const uint32_t pa1 = R3D_ATTR_ALPHA(t->attr) + 1u;
	const int trans = (t->attr & R3D_ATTR_TRANS) != 0, dw = !trans || (t->attr & R3D_ATTR_DEPTHWRITE);

	for (int64_t y = y0; y <= y1; y++) {
		for (int64_t x = x0; x <= x1; x++) {
			const int64_t px = x * 16 + 8, py = y * 16 + 8;
			int64_t e[3];
			int in_tri = 1;
			for (int k = 0; k < 3; k++) {
				e[k] = dx[k] * (py - ay[k]) - dy[k] * (px - ax[k]);
				in_tri &= e[k] - bias[k] >= 0;
			}
			if (!in_tri)
				continue;
			double l[3];
			for (int k = 0; k < 3; k++)
				l[k] = (double)e[k] / (double)area;
			const size_t p = (size_t)(y * W + x);
			double Z = l[0] * z[0] + l[1] * z[1] + l[2] * z[2];
			uint32_t zi = Z <= 0.0 ? 0u : (uint32_t)floor(Z);
			if (!(zi < depth[p]))
				continue;
			uint32_t col;
			if (in->idmode) {
				col = id;
			} else {
				double Q = l[0] * q[0] + l[1] * q[1] + l[2] * q[2];
				int32_t si = (int32_t)floor((l[0] * sq[0] + l[1] * sq[1] + l[2] * sq[2]) / Q);
				int32_t ti = (int32_t)floor((l[0] * tq[0] + l[1] * tq[1] + l[2] * tq[2]) / Q);
				if (!rs) {
					int32_t lo = 2048 * 4, hi = (int32_t)((2048u + w) * 4u - 1u);
					si = si < lo ? lo : si > hi ? hi : si;
				}
				if (!rt) {
					int32_t lo = (int32_t)(2048u * 4u * w), hi = (int32_t)((2048u + h) * 4u * w - 1u);
					ti = ti < lo ? lo : ti > hi ? hi : ti;
				}
				uint32_t tex = WHITE;
				if (textured) {
					uint32_t sx = ((uint32_t)si & ((w - 1u) << 2)) >> 2;
					uint32_t ty = ((uint32_t)ti & ((h - 1u) << (lw + 2u))) >> (lw + 2u);
					tex = ref_texel(in, param, t->pltt, sx, ty);
				}
				uint32_t ch[3];
				for (int c = 0; c < 3; c++) {
					uint32_t cv = (uint32_t)floor((l[0] * cq[c][0] + l[1] * cq[c][1] + l[2] * cq[c][2]) / Q);
					ch[c] = (((tex >> (8 * c)) & 0xFFu) * cv - 1u) >> 6;
				}
				uint32_t a = ((tex >> 24) * pa1 - 1u) >> 5;
				if (a == 0)
					continue;
				col = r3d_rgba(ch[0], ch[1], ch[2], a);
				if (trans) {
					uint32_t d = color[p], da = d >> 24;
					if (da) {
						uint32_t b[3];
						for (int c = 0; c < 3; c++)
							b[c] = (ch[c] * (a + 1u) + ((d >> (8 * c)) & 0xFFu) * (31u - a)) >> 5;
						col = r3d_rgba(b[0], b[1], b[2], a > da ? a : da);
					}
				}
			}
			color[p] = col;
			if (dw)
				depth[p] = zi;
		}
	}
}

void r3d_ref_render(const struct r3d_ref_in *in, uint32_t *color, uint32_t *depth)
{
	const size_t n = (size_t)R3D_W1 * in->scale * R3D_H1 * in->scale;
	for (size_t i = 0; i < n; i++) {
		color[i] = in->clear_color;
		depth[i] = in->clear_depth;
	}
	for (uint32_t i = 0; i < in->ntris; i++)
		ref_tri(in, &in->tris[i], i + 1, color, depth);
}

void r3d_compare(const uint32_t *got, const uint32_t *want, uint32_t n, struct r3d_cmp *c)
{
	*c = (struct r3d_cmp){ 0, 0, 0, n, 0 };
	for (uint32_t i = 0; i < n; i++) {
		if (got[i] == want[i]) {
			c->exact++;
			continue;
		}
		uint32_t m = 0;
		for (int b = 0; b < 32; b += 8) {
			int d = (int)((got[i] >> b) & 0xFFu) - (int)((want[i] >> b) & 0xFFu);
			uint32_t ad = (uint32_t)(d < 0 ? -d : d);
			m = ad > m ? ad : m;
		}
		c->maxdiff = m > c->maxdiff ? m : c->maxdiff;
		if (m <= R3D_CLOSE) {
			c->close++;
		} else {
			if (!c->far)
				c->first_far = i;
			c->far++;
		}
	}
}

uint64_t r3d_hash(const uint32_t *p, uint32_t n)
{
	uint64_t h = 1469598103934665603ull;
	for (uint32_t i = 0; i < n; i++)
		for (int b = 0; b < 32; b += 8) {
			h ^= (p[i] >> b) & 0xFFu;
			h *= 1099511628211ull;
		}
	return h;
}
