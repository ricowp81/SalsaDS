/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.4: núcleo del rasterizador (portable). Ver r3d_core.h.
 *
 * Qué decide qué píxel se dibuja va en enteros: las funciones de borde (con
 * la regla de arriba-izquierda), el recorrido y los recortes. Lo que solo da
 * el color va en flotante de 4 carriles: z, q = 1/w y los atributos
 * divididos por w, con un recíproco por píxel para la corrección de
 * perspectiva. Cada línea se acota con las intersecciones de sus tres bordes
 * (en flotante, con un píxel de margen) y se recorre en pares de grupos de 4
 * píxeles alineados: dos cadenas independientes por vuelta para que el SPU
 * (en orden, latencias de 6–7 ciclos) tenga con qué llenar sus dos tubos.
 *
 * Direccionamiento de texturas sin desplazamientos ni restas por píxel: s se
 * interpola ya como (s + 2048)·4 y t como (t + 2048)·4·ancho (en texeles), así
 * que el desplazamiento en bytes del texel es (S & ((w−1) << 2)) |
 * (T & ((h−1) << (log2 w + 2))); los 2048 texeles de desfase (múltiplo de
 * cualquier tamaño de textura de la DS) hacen que el truncamiento hacia cero
 * sea el piso. Con «clamp» se recorta antes de enmascarar.
 */
#include "r3d_core.h"

#include <string.h>

#include "r3d_io.h"
#include "r3d_vec.h"

#define A128 __attribute__((aligned(128)))
#define A16  __attribute__((aligned(16)))

enum { TAG_IN0 = 0, TAG_IN1 = 1, TAG_OUT0 = 2, TAG_OUT1 = 3, TAG_TEX = 4 };
#define TAG_ALL 0x1Fu

#define TEX_RAW_CHUNK 2048u
#define BIGF 30000.0f

/* ---- Local store ----------------------------------------------------------- */

static struct r3d_tri tri_buf[2][R3D_CHUNK] A128;           /*   4 KB */
static struct r3d_rec rec_buf[2][R3D_CHUNK] A128;           /*  12 KB */
/* +4 palabras: el segundo grupo de un par puede caer, enmascarado, justo
 * después de la última línea. */
static uint32_t color_buf[2][R3D_UNIT_PIXELS + 4] A128;     /*  32 KB */
static uint32_t depth_buf[R3D_UNIT_PIXELS + 4] A128;        /*  16 KB */
static int16_t bb_x0[R3D_MAX_TRIS] A16, bb_x1[R3D_MAX_TRIS] A16;   /* 16 KB */
static int16_t bb_y0[R3D_MAX_TRIS] A16, bb_y1[R3D_MAX_TRIS] A16;
static uint16_t unit_list[R3D_MAX_TRIS] A16;                /*   4 KB */
static uint8_t tex_arena[R3D_TEX_PAGES * R3D_TEX_PAGE] A128;  /* 112 KB */
static uint8_t tex_raw[TEX_RAW_CHUNK + 32] A128;
static uint8_t pal_raw[512 + 32] A128;
static uint32_t pal_dec[256];
/* Textura blanca de 1×1 para los polígonos sin textura o con una que no se
 * pudo cargar. */
static const uint32_t white_texel[4] A16 = { 64u | 64u << 8 | 64u << 16 | 32u << 24 };

/* ---- Geometría del cuadro --------------------------------------------------- */

struct geom {
	uint32_t W, H, TW, TH, ncols, nrows;
	uint32_t row_lo, row_hi;  /* reparto por mitades: filas [lo, hi) */
};

static int row_mine(const struct r3d_job *j, const struct geom *g, uint32_t row)
{
	if (j->split == R3D_SPLIT_HALVES)
		return row >= g->row_lo && row < g->row_hi;
	return row % j->spe_count == j->spe_index;
}

static int rows_mine(const struct r3d_job *j, const struct geom *g, uint32_t r0, uint32_t r1)
{
	if (j->split == R3D_SPLIT_HALVES)
		return r1 >= g->row_lo && r0 < g->row_hi;
	if (r1 - r0 + 1 >= j->spe_count)
		return 1;
	for (uint32_t r = r0; r <= r1; r++)
		if (r % j->spe_count == j->spe_index)
			return 1;
	return 0;
}

static void reject(uint32_t i)
{
	bb_x0[i] = bb_y0[i] = 0x7FFF;
	bb_x1[i] = bb_y1[i] = -1;
}

/* ---- Setup ------------------------------------------------------------------ */

static inline int32_t min3(int32_t a, int32_t b, int32_t c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }
static inline int32_t max3(int32_t a, int32_t b, int32_t c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }

/* Prepara el registro del triángulo `index` y su caja en unidades. 0 si no
 * se dibuja (área nula, fuera del cuadro). */
static int setup(const struct r3d_tri *t, uint32_t index, const struct r3d_job *j, const struct geom *g,
		 struct r3d_rec *r, int16_t bb[4])
{
	const int32_t sc = (int32_t)j->scale;
	int32_t area = (t->x[1] - t->x[0]) * (t->y[2] - t->y[0]) - (t->x[2] - t->x[0]) * (t->y[1] - t->y[0]);
	if (area == 0)
		return 0;
	/* Orden con área positiva: dentro, las tres funciones de borde > 0. */
	const int o1 = area > 0 ? 1 : 2, o2 = 3 - o1;
	const int o[3] = { 0, o1, o2 };
	int32_t X[3], Y[3];
	for (int k = 0; k < 3; k++) {
		X[k] = t->x[o[k]] * sc;
		Y[k] = t->y[o[k]] * sc;
	}
	area = (area < 0 ? -area : area) * sc * sc;

	/* Píxel x cubierto solo si su centro x·16 + 8 cae entre los vértices. */
	int32_t xmin = (min3(X[0], X[1], X[2]) + 7) >> 4, xmax = (max3(X[0], X[1], X[2]) - 8) >> 4;
	int32_t ymin = (min3(Y[0], Y[1], Y[2]) + 7) >> 4, ymax = (max3(Y[0], Y[1], Y[2]) - 8) >> 4;
	if (xmin < 0)
		xmin = 0;
	if (ymin < 0)
		ymin = 0;
	if (xmax > (int32_t)g->W - 1)
		xmax = (int32_t)g->W - 1;
	if (ymax > (int32_t)g->H - 1)
		ymax = (int32_t)g->H - 1;
	if (xmin > xmax || ymin > ymax)
		return 0;
	const int32_t xbase = xmin & ~3, ybase = ymin;
	const int32_t px = xbase * 16 + 8, py = ybase * 16 + 8;

	/* Borde k: de v[k+1] a v[k+2], opuesto al vértice k (su peso). */
	int32_t er[3];
	for (int k = 0; k < 3; k++) {
		const int a = k == 2 ? 0 : k + 1, b = k == 0 ? 2 : k - 1;
		const int32_t dx = X[b] - X[a], dy = Y[b] - Y[a];
		const int32_t ea = -dy * 16, eb = dx * 16;
		er[k] = dx * (py - Y[a]) - dy * (px - X[a]);
		r->ea[k] = ea;
		r->eb[k] = eb;
		r->e0[k] = er[k] - !(ea > 0 || (ea == 0 && eb > 0));  /* arriba-izquierda */
		r->inv_ea[k] = ea ? 1.0f / (float)ea : 0.0f;
	}
	r->ea[3] = r->eb[3] = r->e0[3] = 0;
	r->inv_ea[3] = 0.0f;

	/* Atributos por vértice; q = 1/w (w en 20.12) y el recíproco del área en
	 * un solo recíproco de 4 carriles. */
	const uint32_t texw = R3D_TEX_SIZE_S(t->texparam);
	const vf4 rc = v_recip((vf4){ (float)t->w[o[0]] * (1.0f / 4096.0f), (float)t->w[o[1]] * (1.0f / 4096.0f),
				      (float)t->w[o[2]] * (1.0f / 4096.0f), (float)area });
	vf4 lo = { 0, 0, 0, 0 }, hi = lo, dxl = lo, dxh = lo, dyl = lo, dyh = lo;
	for (int k = 0; k < 3; k++) {
		const int v = o[k];
		const float q = rc[k];
		const float s = ((float)t->s[v] * (1.0f / 16.0f) + 2048.0f) * 4.0f;
		const float tt = ((float)t->t[v] * (1.0f / 16.0f) + 2048.0f) * 4.0f * (float)texw;
		const uint32_t c = t->rgb[v];
		const vf4 al = { (float)t->z[v], q, s * q, tt * q };
		/* (c + 1) + 0,5: el truncamiento redondea y nunca baja de 1. */
		const vf4 ah = { ((float)r3d_x6(c & 31u) + 1.5f) * q, ((float)r3d_x6((c >> 5) & 31u) + 1.5f) * q,
				 ((float)r3d_x6((c >> 10) & 31u) + 1.5f) * q, 0.0f };
		const vf4 fe = v_splatf((float)er[k]), fa = v_splatf((float)r->ea[k]), fb = v_splatf((float)r->eb[k]);
		lo += al * fe;
		hi += ah * fe;
		dxl += al * fa;
		dxh += ah * fa;
		dyl += al * fb;
		dyh += ah * fb;
	}
	const vf4 ia = v_splatf(rc[3]);
	*(vf4 *)&r->abase[0] = lo * ia;
	*(vf4 *)&r->abase[4] = hi * ia;
	*(vf4 *)&r->adx[0] = dxl * ia;
	*(vf4 *)&r->adx[4] = dxh * ia;
	*(vf4 *)&r->ady[0] = dyl * ia;
	*(vf4 *)&r->ady[4] = dyh * ia;

	r->xmin = (int16_t)xmin;
	r->xmax = (int16_t)xmax;
	r->ymin = (int16_t)ymin;
	r->ymax = (int16_t)ymax;
	r->xbase = (int16_t)xbase;
	r->ybase = (int16_t)ybase;
	r->attr = t->attr;
	r->pltt = t->pltt;
	r->texparam = t->texparam;
	r->id = index + 1;

	bb[0] = (int16_t)((uint32_t)xmin / g->TW);
	bb[1] = (int16_t)((uint32_t)xmax / g->TW);
	bb[2] = (int16_t)((uint32_t)ymin / g->TH);
	bb[3] = (int16_t)((uint32_t)ymax / g->TH);
	return rows_mine(j, g, (uint32_t)bb[2], (uint32_t)bb[3]);
}

/* ---- Caché de texturas ------------------------------------------------------- */

/*
 * Texturas enteras ya decodificadas (32 bits por texel) en páginas de 4 KB
 * contiguas del local store. Clave: offset, tamaño, formato y transparencia
 * del índice 0 de TEXIMAGE_PARAM (repetir/espejar no cambian los texeles),
 * más PLTT_BASE si tiene paleta. Reemplazo: la corrida de páginas cuyo uso
 * más reciente es el más viejo. Se vacía al empezar cada cuadro (la VRAM
 * pudo cambiar; un emulador llevaría versiones por banco).
 */
#define TEX_KEY_MASK (0xFFFFu | 0x3Fu << 20 | 7u << 26 | R3D_TEX_COLOR0)

struct texent {
	uint32_t key, pltt, stamp;
	uint16_t page, npages;
	int valid;
};

struct texview {
	const uint8_t *base;
	vu4 smask, tmask;
	vi4 slo, shi, tlo, thi;
	int general;  /* algún eje con «clamp» */
};

static struct texent tex_ent[R3D_TEX_ENTRIES];
static int8_t page_owner[R3D_TEX_PAGES];
static uint32_t tex_clock;
static int tex_last;

static void tex_reset(void)
{
	for (uint32_t i = 0; i < R3D_TEX_ENTRIES; i++)
		tex_ent[i].valid = 0;
	for (uint32_t p = 0; p < R3D_TEX_PAGES; p++)
		page_owner[p] = -1;
	tex_clock = 0;
	tex_last = -1;
}

static void tex_evict(int e)
{
	for (uint32_t p = tex_ent[e].page; p < (uint32_t)tex_ent[e].page + tex_ent[e].npages; p++)
		page_owner[p] = -1;
	tex_ent[e].valid = 0;
}

/* Trae `len` bytes desde una dirección cualquiera (el tramo alineado a 16
 * que los contiene) y devuelve el puntero al primero. Espera. */
static const uint8_t *fetch(uint8_t *buf, uint64_t ea, uint32_t len)
{
	uint64_t start = ea & ~15ull;
	uint32_t total = (uint32_t)(((ea + len + 15) & ~15ull) - start);
	r3d_io_get(buf, start, total, TAG_TEX);
	r3d_io_wait(1u << TAG_TEX);
	return buf + (ea - start);
}

static int tex_load(const struct r3d_job *j, uint32_t param, uint32_t pltt, uint32_t *dst, struct r3d_stats *st)
{
	const uint32_t fmt = R3D_TEX_FORMAT(param), bytes = r3d_tex_bytes(param), off = R3D_TEX_OFFSET(param);
	if (off + bytes > j->vram_tex_size)
		return 0;
	const uint32_t pb = r3d_pal_bytes(param);
	if (pb) {
		const uint32_t poff = (pltt & 0x1FFFu) * 16u;
		if (poff + pb > j->vram_pal_size)
			return 0;
		const uint8_t *p = fetch(pal_raw, j->vram_pal_ea + poff, pb);
		for (uint32_t i = 0; i < pb / 2; i++)
			pal_dec[i] = r3d_texel((uint32_t)p[2 * i] | (uint32_t)p[2 * i + 1] << 8, 31);
		if (param & R3D_TEX_COLOR0)
			pal_dec[0] = r3d_texel((uint32_t)p[0] | (uint32_t)p[1] << 8, 0);
	}
	for (uint32_t pos = 0; pos < bytes; pos += TEX_RAW_CHUNK) {
		const uint32_t len = bytes - pos < TEX_RAW_CHUNK ? bytes - pos : TEX_RAW_CHUNK;
		const uint8_t *s = fetch(tex_raw, j->vram_tex_ea + off + pos, len);
		switch (fmt) {
		case R3D_FMT_DIRECT:
			for (uint32_t i = 0; i < len; i += 2) {
				uint32_t c = (uint32_t)s[i] | (uint32_t)s[i + 1] << 8;
				*dst++ = r3d_texel(c & 0x7FFFu, c & 0x8000u ? 31u : 0u);
			}
			break;
		case R3D_FMT_PAL256:
			for (uint32_t i = 0; i < len; i++)
				*dst++ = pal_dec[s[i]];
			break;
		default: /* R3D_FMT_PAL16: el texel de la izquierda en el nibble bajo */
			for (uint32_t i = 0; i < len; i++) {
				*dst++ = pal_dec[s[i] & 15u];
				*dst++ = pal_dec[s[i] >> 4];
			}
			break;
		}
	}
	st->tex_bytes += bytes + pb;
	return 1;
}

static void tex_white(struct texview *tv)
{
	tv->base = (const uint8_t *)white_texel;
	tv->smask = tv->tmask = v_splatu(0);
	tv->slo = tv->tlo = v_splati(INT32_MIN);
	tv->shi = tv->thi = v_splati(INT32_MAX);
	tv->general = 0;
}

static void tex_get(const struct r3d_job *j, uint32_t param, uint32_t pltt, struct texview *tv, struct r3d_stats *st)
{
	const uint32_t fmt = R3D_TEX_FORMAT(param), bytes = r3d_tex_bytes(param);
	const uint32_t lw = 3u + ((param >> 20) & 7u), w = 1u << lw, h = R3D_TEX_SIZE_T(param);
	const uint32_t npages = (w * h * 4u + R3D_TEX_PAGE - 1) / R3D_TEX_PAGE;
	if (!bytes || npages > R3D_TEX_PAGES) {
		if (fmt != R3D_FMT_NONE)
			st->tex_fail++;
		tex_white(tv);
		return;
	}
	const uint32_t key = param & TEX_KEY_MASK, pk = r3d_pal_bytes(param) ? pltt : 0;
	int e = tex_last;
	if (e < 0 || !tex_ent[e].valid || tex_ent[e].key != key || tex_ent[e].pltt != pk) {
		e = -1;
		for (int i = 0; i < (int)R3D_TEX_ENTRIES; i++)
			if (tex_ent[i].valid && tex_ent[i].key == key && tex_ent[i].pltt == pk) {
				e = i;
				break;
			}
	}
	if (e < 0) {
		const uint32_t t0 = r3d_io_ticks();
		/* Una entrada libre o la menos usada. */
		int slot = -1;
		for (int i = 0; i < (int)R3D_TEX_ENTRIES; i++)
			if (!tex_ent[i].valid) {
				slot = i;
				break;
			}
		if (slot < 0) {
			slot = 0;
			for (int i = 1; i < (int)R3D_TEX_ENTRIES; i++)
				if (tex_ent[i].stamp < tex_ent[slot].stamp)
					slot = i;
			tex_evict(slot);
		}
		/* La corrida de páginas más vieja. */
		uint32_t best = 0, best_cost = UINT32_MAX;
		for (uint32_t s = 0; s + npages <= R3D_TEX_PAGES; s++) {
			uint32_t cost = 0;
			for (uint32_t p = s; p < s + npages; p++)
				if (page_owner[p] >= 0 && tex_ent[page_owner[p]].stamp > cost)
					cost = tex_ent[page_owner[p]].stamp;
			if (cost < best_cost) {
				best_cost = cost;
				best = s;
			}
		}
		for (uint32_t p = best; p < best + npages; p++)
			if (page_owner[p] >= 0)
				tex_evict(page_owner[p]);
		st->tex_miss++;
		if (!tex_load(j, param, pltt, (uint32_t *)(tex_arena + best * R3D_TEX_PAGE), st)) {
			st->tex_fail++;
			tex_white(tv);
			st->t_tex += r3d_io_ticks() - t0;
			return;
		}
		for (uint32_t p = best; p < best + npages; p++)
			page_owner[p] = (int8_t)slot;
		tex_ent[slot] = (struct texent){ key, pk, 0, (uint16_t)best, (uint16_t)npages, 1 };
		e = slot;
		st->t_tex += r3d_io_ticks() - t0;
	}
	tex_ent[e].stamp = ++tex_clock;
	tex_last = e;

	tv->base = tex_arena + tex_ent[e].page * R3D_TEX_PAGE;
	tv->smask = v_splatu((w - 1u) << 2);
	tv->tmask = v_splatu((h - 1u) << (lw + 2u));
	const int rs = (param & R3D_TEX_REPEAT_S) != 0, rt = (param & R3D_TEX_REPEAT_T) != 0;
	tv->slo = v_splati(rs ? INT32_MIN : (int32_t)(2048u * 4u));
	tv->shi = v_splati(rs ? INT32_MAX : (int32_t)((2048u + w) * 4u - 1u));
	tv->tlo = v_splati(rt ? INT32_MIN : (int32_t)(2048u * 4u * w));
	tv->thi = v_splati(rt ? INT32_MAX : (int32_t)((2048u + h) * 4u * w - 1u));
	tv->general = !(rs && rt);
}

/* ---- Dibujo ----------------------------------------------------------------- */

struct unit {
	int32_t x0, y0, x1, y1;  /* píxeles [x0, x1) × [y0, y1) */
	int32_t w;               /* ancho de la unidad (paso de las líneas) */
	uint32_t *color, *depth;
};

struct tri_k {
	vf4 dz, dq, ds, dt, dr, dg, db;
	vu4 pa1, idv, dwm;
};

struct row_k {
	vf4 z, q, s, t, r, g, b;
};

R3D_INLINE vf4 v_hmax(vf4 v)
{
	v = v_maxf(v, __builtin_shuffle(v, (vi4){ 2, 3, 0, 1 }));
	return v_maxf(v, __builtin_shuffle(v, (vi4){ 1, 0, 3, 2 }));
}

R3D_INLINE vf4 v_hmin(vf4 v)
{
	v = v_minf(v, __builtin_shuffle(v, (vi4){ 2, 3, 0, 1 }));
	return v_minf(v, __builtin_shuffle(v, (vi4){ 1, 0, 3, 2 }));
}

/* Un grupo de 4 píxeles alineado: cp y zp apuntan a su color y profundidad;
 * e0–e2 son sus funciones de borde, xr su x relativa a la base del
 * triángulo y m los carriles permitidos. */
R3D_INLINE void quad(uint32_t *cp, uint32_t *zp, vi4 e0, vi4 e1, vi4 e2, vf4 xr, vu4 m, const struct tri_k *k,
		     const struct row_k *rk, const struct texview *tv, vu4 *frag, const int trans, const int gen,
		     const int idm)
{
	const vu4 one = v_splatu(1);
	const vu4 cov = v_ge0(e0 | e1 | e2) & m;
	const vu4 zi = v_cvt_u(k->dz * xr + rk->z);
	const vu4 zb = *(const vu4 *)zp;
	const vu4 old = *(const vu4 *)cp;
	vu4 pass = cov & v_gt_u(zb, zi);  /* z < z-buffer */
	vu4 col;
	if (idm) {
		col = k->idv;
	} else {
		const vf4 w = v_recip(k->dq * xr + rk->q);
		vi4 si = v_cvt_i((k->ds * xr + rk->s) * w);
		vi4 ti = v_cvt_i((k->dt * xr + rk->t) * w);
		if (gen) {
			si = v_mini(v_maxi(si, tv->slo), tv->shi);
			ti = v_mini(v_maxi(ti, tv->tlo), tv->thi);
		}
		const vu4 tex = v_gather32(tv->base, ((vu4)si & tv->smask) | ((vu4)ti & tv->tmask));
		const vu4 cr = v_cvt_u((k->dr * xr + rk->r) * w);
		const vu4 cg = v_cvt_u((k->dg * xr + rk->g) * w);
		const vu4 cb = v_cvt_u((k->db * xr + rk->b) * w);
		/* Modulación de la DS: ((texel + 1)·(vértice + 1) − 1) / 64 en 6 bits;
		 * alfa ((texel + 1)·(polígono + 1) − 1) / 32 en 5 bits. */
		const vu4 r = (v_mul16(v_byte0(tex), cr) - one) >> 6;
		const vu4 g = (v_mul16(v_byte1(tex), cg) - one) >> 6;
		const vu4 b = (v_mul16(v_byte2(tex), cb) - one) >> 6;
		const vu4 a = (v_mul16(v_byte3(tex), k->pa1) - one) >> 5;
		pass &= v_gt_u(a, v_splatu(0));  /* alfa 0: no se dibuja */
		if (trans) {
			/* (fuente·(a+1) + destino·(31−a)) / 32; sobre alfa 0, sin mezclar. */
			const vu4 ws = a + one, wd = v_splatu(31) - a;
			const vu4 br = (v_mul16(r, ws) + v_mul16(v_byte0(old), wd)) >> 5;
			const vu4 bg = (v_mul16(g, ws) + v_mul16(v_byte1(old), wd)) >> 5;
			const vu4 bb = (v_mul16(b, ws) + v_mul16(v_byte2(old), wd)) >> 5;
			const vu4 da = v_byte3(old);
			col = v_selu(v_pack4(r, g, b, a), v_pack4(br, bg, bb, v_maxu(a, da)), v_gt_u(da, v_splatu(0)));
		} else {
			col = v_pack4(r, g, b, a);
		}
	}
	*(vu4 *)cp = v_selu(old, col, pass);
	*(vu4 *)zp = v_selu(zb, zi, trans ? pass & k->dwm : pass);
	*frag -= pass;
}

R3D_INLINE void draw_tri(const struct r3d_rec *r, const struct texview *tv, const struct unit *u,
			 struct r3d_stats *st, const int trans, const int gen, const int idm)
{
	const int32_t ya = r->ymin > u->y0 ? r->ymin : u->y0, yb = r->ymax < u->y1 - 1 ? r->ymax : u->y1 - 1;
	const int32_t xlo = r->xmin > u->x0 ? r->xmin : u->x0, xhi = r->xmax < u->x1 - 1 ? r->xmax : u->x1 - 1;
	if (ya > yb || xlo > xhi)
		return;
	const vi4 ea = *(const vi4 *)r->ea, eb = *(const vi4 *)r->eb;
	const vf4 inv = *(const vf4 *)r->inv_ea;
	const vf4 abl = *(const vf4 *)&r->abase[0], abh = *(const vf4 *)&r->abase[4];
	const vf4 dxl = *(const vf4 *)&r->adx[0], dxh = *(const vf4 *)&r->adx[4];
	const vf4 dyl = *(const vf4 *)&r->ady[0], dyh = *(const vf4 *)&r->ady[4];
	struct tri_k k;
	k.dz = v_splatf(dxl[0]);
	k.dq = v_splatf(dxl[1]);
	k.ds = v_splatf(dxl[2]);
	k.dt = v_splatf(dxl[3]);
	k.dr = v_splatf(dxh[0]);
	k.dg = v_splatf(dxh[1]);
	k.db = v_splatf(dxh[2]);
	k.pa1 = v_splatu(R3D_ATTR_ALPHA(r->attr) + 1u);
	k.idv = v_splatu(r->id);
	k.dwm = v_splatu(r->attr & R3D_ATTR_DEPTHWRITE ? ~0u : 0u);

	const vi4 lane = { 0, 1, 2, 3 };
	const vi4 l0 = v_mul32x16(v_splati(ea[0]), lane), l1 = v_mul32x16(v_splati(ea[1]), lane),
		  l2 = v_mul32x16(v_splati(ea[2]), lane);
	const vi4 s0 = v_splati(ea[0] * 4), s1 = v_splati(ea[1] * 4), s2 = v_splati(ea[2] * 4);
	const vu4 pos = v_gt_s(ea, v_splati(0)), neg = v_gt_s(v_splati(0), ea), zer = ~(pos | neg);
	const vf4 xbf = v_splatf((float)r->xbase);
	const vf4 lanef = { 0.0f, 1.0f, 2.0f, 3.0f };

	vi4 erow = *(const vi4 *)r->e0 + v_mul32x16(eb, v_splati(ya - r->ybase));
	vu4 frag = v_splatu(0);
	uint32_t quads = 0, rows = 0;
	for (int32_t y = ya; y <= yb; y++, erow += eb) {
		/* Intersección de la línea con cada borde: x = xbase − E/ea. */
		const vf4 t = xbf - v_cvt_f(erow) * inv;
		vf4 lo = v_self(v_splatf(-BIGF), t, pos);
		lo = v_self(lo, v_splatf(BIGF), zer & v_gt_s(v_splati(0), erow));  /* borde horizontal por fuera */
		const vf4 hi = v_hmin(v_self(v_splatf(BIGF), t, neg));
		lo = v_hmax(lo);
		int32_t xa = (int32_t)(lo[0] + 65536.0f) - 65536 - 1;
		int32_t xb = (int32_t)(hi[0] + 65536.0f) - 65536 + 1;
		if (xa < xlo)
			xa = xlo;
		if (xb > xhi)
			xb = xhi;
		if (xa > xb)
			continue;
		xa &= ~3;
		rows++;

		const int32_t nq = xa - r->xbase;
		const vf4 dy = v_splatf((float)(y - r->ybase));
		const vf4 rl = abl + dyl * dy, rh = abh + dyh * dy;
		const struct row_k rk = { v_splatf(rl[0]), v_splatf(rl[1]), v_splatf(rl[2]), v_splatf(rl[3]),
					  v_splatf(rh[0]), v_splatf(rh[1]), v_splatf(rh[2]) };
		const vi4 eq = erow + v_mul32x16(ea, v_splati(nq));
		vi4 e0 = v_splati(eq[0]) + l0, e1 = v_splati(eq[1]) + l1, e2 = v_splati(eq[2]) + l2;
		vf4 xr = v_splatf((float)nq) + lanef;
		uint32_t *crow = u->color + (y - u->y0) * u->w, *zrow = u->depth + (y - u->y0) * u->w;
		for (int32_t x = xa; x <= xb; x += 8) {
			const int32_t i = x - u->x0;
			const vu4 m2 = v_splatu(x + 4 <= xb ? ~0u : 0u);
			quad(crow + i, zrow + i, e0, e1, e2, xr, v_splatu(~0u), &k, &rk, tv, &frag, trans, gen, idm);
			quad(crow + i + 4, zrow + i + 4, e0 + s0, e1 + s1, e2 + s2, xr + v_splatf(4.0f), m2, &k, &rk, tv,
			     &frag, trans, gen, idm);
			e0 += s0 + s0;
			e1 += s1 + s1;
			e2 += s2 + s2;
			xr += v_splatf(8.0f);
			quads += 2;
		}
	}
	st->rows += rows;
	st->quads += quads;
	st->frags += frag[0] + frag[1] + frag[2] + frag[3];
}

static void draw_rec(const struct r3d_rec *r, const struct texview *tv, const struct unit *u, struct r3d_stats *st,
		     int idm)
{
	const int trans = (r->attr & R3D_ATTR_TRANS) != 0;
	if (idm) {
		if (trans)
			draw_tri(r, tv, u, st, 1, 0, 1);
		else
			draw_tri(r, tv, u, st, 0, 0, 1);
		return;
	}
	switch (trans << 1 | tv->general) {
	case 0: draw_tri(r, tv, u, st, 0, 0, 0); break;
	case 1: draw_tri(r, tv, u, st, 0, 1, 0); break;
	case 2: draw_tri(r, tv, u, st, 1, 0, 0); break;
	default: draw_tri(r, tv, u, st, 1, 1, 0); break;
	}
}

/* ---- Pasadas ------------------------------------------------------------------ */

static inline uint32_t chunk_len(uint32_t n, uint32_t c)
{
	uint32_t left = n - c * R3D_CHUNK;
	return left < R3D_CHUNK ? left : R3D_CHUNK;
}

static void pass_setup(const struct r3d_job *j, const struct geom *g, struct r3d_stats *st)
{
	const uint32_t t0 = r3d_io_ticks();
	const uint32_t n = j->ntris, nch = (n + R3D_CHUNK - 1) / R3D_CHUNK;
	for (uint32_t i = n; i < ((n + 7u) & ~7u); i++)
		reject(i);
	if (nch)
		r3d_io_get(tri_buf[0], j->tris_ea, chunk_len(n, 0) * sizeof(struct r3d_tri), TAG_IN0);
	for (uint32_t c = 0; c < nch; c++) {
		const uint32_t b = c & 1u, cnt = chunk_len(n, c);
		r3d_io_wait(1u << (TAG_IN0 + b));
		if (c + 1 < nch)
			r3d_io_get(tri_buf[b ^ 1u], j->tris_ea + (uint64_t)(c + 1) * R3D_CHUNK * sizeof(struct r3d_tri),
				   chunk_len(n, c + 1) * sizeof(struct r3d_tri), TAG_IN0 + (b ^ 1u));
		r3d_io_wait(1u << (TAG_OUT0 + b));  /* los registros del lote c − 2 ya salieron */
		for (uint32_t k = 0; k < cnt; k++) {
			const uint32_t i = c * R3D_CHUNK + k;
			int16_t bb[4];
			if (setup(&tri_buf[b][k], i, j, g, &rec_buf[b][k], bb)) {
				bb_x0[i] = bb[0];
				bb_x1[i] = bb[1];
				bb_y0[i] = bb[2];
				bb_y1[i] = bb[3];
				st->tris_mine++;
			} else {
				reject(i);
			}
		}
		r3d_io_put(rec_buf[b], j->scratch_ea + (uint64_t)c * R3D_CHUNK * R3D_REC_SIZE, cnt * R3D_REC_SIZE,
			   TAG_OUT0 + b);
	}
	/* La pasada 2 lee de memoria principal lo que acaba de salir. */
	r3d_io_wait((1u << TAG_OUT0) | (1u << TAG_OUT1));
	st->t_setup = r3d_io_ticks() - t0;
}

/* Triángulos que tocan la unidad (ux, uy), en orden de dibujo; 8 a la vez. */
static uint32_t build_list(int16_t ux, int16_t uy, uint32_t n8)
{
	const vs8 X = v_splats16(ux), Y = v_splats16(uy);
	uint32_t cnt = 0;
	for (uint32_t i = 0; i < n8; i += 8) {
		const vs8 out = v_gt_s16(*(const vs8 *)&bb_x0[i], X) | v_gt_s16(X, *(const vs8 *)&bb_x1[i]) |
				v_gt_s16(*(const vs8 *)&bb_y0[i], Y) | v_gt_s16(Y, *(const vs8 *)&bb_y1[i]);
		uint32_t bits = ~v_bits16(out) & 0xFFu;
		while (bits) {
			const uint32_t lane = (uint32_t)__builtin_clz(bits) - 24u;
			unit_list[cnt++] = (uint16_t)(i + lane);
			bits &= ~(0x80u >> lane);
		}
	}
	return cnt;
}

static void draw_unit(const struct r3d_job *j, const struct unit *u, uint32_t cnt, struct r3d_stats *st)
{
	const uint32_t nch = (cnt + R3D_CHUNK - 1) / R3D_CHUNK;
	const int idm = (j->flags & R3D_FLAG_ID) != 0;
	if (nch)
		r3d_io_gather(rec_buf[0], j->scratch_ea, unit_list, chunk_len(cnt, 0), R3D_REC_SIZE, TAG_IN0);
	for (uint32_t c = 0; c < nch; c++) {
		const uint32_t b = c & 1u, n = chunk_len(cnt, c);
		uint32_t t = r3d_io_ticks();
		r3d_io_wait(1u << (TAG_IN0 + b));
		st->t_gather += r3d_io_ticks() - t;
		if (c + 1 < nch)
			r3d_io_gather(rec_buf[b ^ 1u], j->scratch_ea, unit_list + (c + 1) * R3D_CHUNK,
				      chunk_len(cnt, c + 1), R3D_REC_SIZE, TAG_IN0 + (b ^ 1u));
		for (uint32_t k = 0; k < n; k++) {
			const struct r3d_rec *r = &rec_buf[b][k];
			struct texview tv;
			if (idm)
				tex_white(&tv);
			else
				tex_get(j, r->texparam, r->pltt, &tv, st);
			t = r3d_io_ticks();
			draw_rec(r, &tv, u, st, idm);
			st->t_raster += r3d_io_ticks() - t;
		}
	}
}

static void clear_unit(uint32_t *color, uint32_t *depth, uint32_t c, uint32_t z)
{
	const vu4 cv = v_splatu(c), zv = v_splatu(z);
	vu4 *cp = (vu4 *)color, *zp = (vu4 *)depth;
	for (uint32_t i = 0; i < (R3D_UNIT_PIXELS + 4) / 4; i++) {
		cp[i] = cv;
		zp[i] = zv;
	}
}

static void pass_draw(const struct r3d_job *j, const struct geom *g, struct r3d_stats *st)
{
	const uint32_t n8 = (j->ntris + 7u) & ~7u;
	uint32_t ci = 0;
	for (uint32_t uy = 0; uy < g->nrows; uy++) {
		if (!row_mine(j, g, uy))
			continue;
		for (uint32_t ux = 0; ux < g->ncols; ux++) {
			uint32_t t0 = r3d_io_ticks();
			r3d_io_wait(1u << (TAG_OUT0 + ci));  /* la salida anterior de este búfer */
			clear_unit(color_buf[ci], depth_buf, j->clear_color, j->clear_depth);
			const struct unit u = { (int32_t)(ux * g->TW), (int32_t)(uy * g->TH), (int32_t)((ux + 1) * g->TW),
						(int32_t)((uy + 1) * g->TH), (int32_t)g->TW, color_buf[ci], depth_buf };
			uint32_t t1 = r3d_io_ticks();
			st->t_out += t1 - t0;
			const uint32_t cnt = build_list((int16_t)ux, (int16_t)uy, n8);
			st->t_bin += r3d_io_ticks() - t1;
			st->units++;
			st->unit_tris += cnt;

			draw_unit(j, &u, cnt, st);

			t0 = r3d_io_ticks();
			const uint64_t ea = j->frame_ea + ((uint64_t)u.y0 * g->W + (uint64_t)u.x0) * 4u;
			if (g->TW == g->W)
				r3d_io_put(color_buf[ci], ea, R3D_UNIT_PIXELS * 4u, TAG_OUT0 + ci);
			else
				r3d_io_put_rows(color_buf[ci], ea, g->TH, g->TW * 4u, g->W * 4u, TAG_OUT0 + ci);
			ci ^= 1u;
			st->t_out += r3d_io_ticks() - t0;
		}
	}
}

int r3d_frame(const struct r3d_job *j, struct r3d_stats *st)
{
	struct geom g;
	memset(st, 0, sizeof *st);
	if (j->scale != 1 && j->scale != 2 && j->scale != 4)
		return R3D_EARG;
	g.W = r3d_frame_w(j->scale);
	g.H = r3d_frame_h(j->scale);
	g.TW = j->unit_w;
	if (g.TW < 256 || g.TW > g.W || g.W % g.TW || R3D_UNIT_PIXELS % g.TW)
		return R3D_EARG;
	g.TH = R3D_UNIT_PIXELS / g.TW;
	if (g.H % g.TH || (g.TW < g.W && g.TH > 16))
		return R3D_EARG;
	g.ncols = g.W / g.TW;
	g.nrows = g.H / g.TH;
	if (!j->spe_count || j->spe_index >= j->spe_count || j->split > R3D_SPLIT_HALVES)
		return R3D_EARG;
	g.row_lo = j->spe_index * g.nrows / j->spe_count;
	g.row_hi = (j->spe_index + 1) * g.nrows / j->spe_count;
	if (j->ntris > R3D_MAX_TRIS)
		return R3D_ETRIS;
	if ((j->tris_ea | j->frame_ea | j->scratch_ea | j->vram_tex_ea | j->vram_pal_ea) & 15u)
		return R3D_EALIGN;

	tex_reset();
	pass_setup(j, &g, st);
	pass_draw(j, &g, st);
	r3d_io_wait(TAG_ALL);
	return 0;
}
