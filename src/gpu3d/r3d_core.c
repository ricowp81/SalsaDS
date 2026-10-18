/*
 * SalsaDS — rasterizador 3D por bandas de líneas (Fase 3.2): el código de los SPE. En el host y en el PPE compila
 * igual con la E/S de r3d_io.h (copias inmediatas que comprueban las reglas del DMA). Las reglas de la DS están en
 * r3d_px.h y son las mismas de la referencia (r3d_ref.c): los dos dan los mismos bits.
 *
 * Un trabajo = un cuadro de geometría (gx_frame en memoria principal) repartido en bandas de `band` líneas entre
 * `nunits` SPE (bandas alternas). Cada SPE:
 *   1. trae los 2048 polígonos a trozos (DMA, doble búfer), saca su clave de orden (r3d_sort_key) y su extensión en y,
 *      y los ordena en el local store (radix estable de 3 pasadas: la clave lleva el índice, el orden es exacto);
 *   2. por banda: limpia color, profundidad y atributos (CLEAR_COLOR, CLEAR_DEPTH), arma la lista de polígonos que la
 *      tocan en orden de dibujo, los trae en lotes de 16 con sus vértices (listas de DMA, el lote siguiente mientras se
 *      dibuja el actual), los dibuja con r3d_px.h, hace edge marking y niebla, y manda la banda a memoria principal;
 *      con edge marking la banda se dibuja con una línea más arriba y otra abajo (los vecinos que mira el borde);
 *   3. texturas: caché de bloques DECODIFICADOS de 256 texeles (4 líneas × 64 texeles, o la textura entera en
 *      franjas si es más angosta) en 64 bloques de 1 KB, asociativa de 4 vías con LRU por conjunto, que se llena por
 *      demanda: el bloque se decodifica (r3d_decode, el mismo de la referencia) leyendo la VRAM por una caché de líneas
 *      de 128 bytes. Se vacía al empezar el cuadro (la VRAM pudo cambiar) y dura todas las bandas del SPE: lo que
 *      desbordaba la caché de texturas enteras de la Fase 0.4 (cada banda recorría todas las texturas) ya no pasa,
 *      porque solo se decodifica lo que se muestrea.
 *
 * Presupuesto del local store del contexto (sizeof(r3d_ctx), lo imprime el SPE; programa en src/spu/raster3d):
 *   color 2 × 18 líneas × 1 KB = 36 KB (doble búfer de salida), profundidad 18 KB, atributos 18 KB,
 *   orden y extensiones 2048 × (4 + 4 + 2 + 2) = 24 KB, lista de la banda 4 KB, lotes 2 × (16 × 48 + 160 × 32) = 11,5 KB,
 *   listas de DMA 2 × 160 × 8 = 2,5 KB, caché de texturas 64 KB + etiquetas 1 KB, líneas de VRAM 64 × 128 = 8 KB.
 *   ≈ 188 KB; el programa y la pila van en el resto (ver la cabecera de src/spu/raster3d/raster3d.c).
 */
#include <stddef.h>
#include <string.h>

#include "gpu3d/r3d.h"
#include "gpu3d/r3d_core.h"
#include "gpu3d/r3d_px.h"

#ifndef __SPU__
uint32_t r3d_io_errors;
#endif

uint32_t r3d_core_ctx_size(void)
{
	return (uint32_t)sizeof(r3d_ctx);
}

/* ---- caché de líneas de VRAM (128 bytes) ----------------------------------------------------------- */

/* espacio 0: texturas (512 KB en 4 ranuras); 1: paletas (96 KB en 6 ranuras) */
static uint64_t vram_line_ea(const r3d_ctx *c, uint32_t space, uint32_t a)
{
	uint32_t off;

	if (space == 0) {
		off = c->job.texslot[(a >> 17) & 3u];
		a &= 0x1FFFFu;
	} else {
		const uint32_t slot = (a >> 14) & 7u;

		off = slot < R3D_PAL_SLOTS ? c->job.palslot[slot] : R3D_UNMAPPED;
		a &= 0x3FFFu;
	}
	if (off == R3D_UNMAPPED)
		return c->job.vram_ea + R3D_VRAM_ZERO_OFF;
	return c->job.vram_ea + off + (a & ~127u);
}

static inline uint32_t lc_rd(r3d_ctx *c, uint32_t space, uint32_t a)
{
	/* asociativas de 2 vías (conjunto = línea & n/2 - 1; se reemplaza la que no se usó por última vez), una para
	 * texturas y otra para paletas: los datos, los índices del 4x4 y la paleta de un mismo texel no se pisan */
	const uint32_t key = a >> 7;
	const uint32_t base = space ? R3D_LC_LINES : 0u, half = space ? R3D_LP_LINES / 2u : R3D_LC_LINES / 2u;
	const uint32_t i0 = base + (key & (half - 1u)), i1 = i0 + half;
	uint32_t i;

	if (c->ltag[i0] == key) {
		c->lru[i0 - base] = 0;
		return c->lines[i0][a & 127u];
	}
	if (c->ltag[i1] == key) {
		c->lru[i0 - base] = 1;
		return c->lines[i1][a & 127u];
	}
	i = c->lru[i0 - base] ? i0 : i1; /* la que no se usó */
	c->lru[i0 - base] = (uint8_t)(i == i1);
	r3d_get(c->lines[i], vram_line_ea(c, space, a), 128, R3D_TAG_LINE);
	r3d_wait(1u << R3D_TAG_LINE);
	c->ltag[i] = key;
	c->st->line_miss++;
	c->st->tex_bytes += 128;
	return c->lines[i][a & 127u];
}

static uint32_t lc_rdt(void *ctx, uint32_t a)
{
	return lc_rd((r3d_ctx *)ctx, 0, a & 0x7FFFFu);
}

static uint32_t lc_rdp(void *ctx, uint32_t a)
{
	return lc_rd((r3d_ctx *)ctx, 1, a & 0x1FFFFu);
}

/* ---- caché de bloques decodificados ------------------------------------------------------------------ */

static inline uint32_t tc_csh(const r3d_tex *tx)
{
	return tx->sl < 6 ? tx->sl : 6u;
}

static const uint32_t *tc_block(r3d_ctx *c, const r3d_tex *tx, uint64_t tag, uint32_t bx, uint32_t by)
{
	const uint32_t set = (uint32_t)((tag ^ (tag >> 13) ^ (tag >> 29) ^ (tag >> 41)) * 0x9E3779B1u >> 28) & (R3D_TC_SETS - 1u);
	uint32_t *blk;
	uint32_t way = 0, oldest = 0xFFFFFFFFu;

	c->clock++;
	for (uint32_t w = 0; w < R3D_TC_WAYS; w++) {
		const uint32_t i = set * R3D_TC_WAYS + w;

		if (c->ttag[i] == tag) {
			c->tstamp[i] = c->clock;
			c->st->tex_hits++;
			return c->tcache[i];
		}
		if (c->tstamp[i] < oldest) {
			oldest = c->tstamp[i];
			way = w;
		}
	}
	{
		const uint32_t i = set * R3D_TC_WAYS + way;
		const uint32_t csh = tc_csh(tx), rsh = 8u - csh;
		const uint32_t cols = 1u << csh, rows = 1u << rsh;
		const uint32_t T = 1u << tx->tl;
		const uint32_t t0 = by << rsh, s0 = bx << csh;
		const uint32_t nr = t0 + rows > T ? T - t0 : rows;
		const uint32_t t_ = R3D_T();

		if (c->ttag[i] != ~0ull)
			c->st->tex_evict++;
		c->st->tex_misses++;
		blk = c->tcache[i];
		for (uint32_t r = 0; r < nr; r++)
			for (uint32_t q = 0; q < cols; q++)
				blk[(r << csh) | q] = r3d_decode(tx, s0 + q, t0 + r, lc_rdt, lc_rdp, c);
		c->ttag[i] = tag;
		c->tstamp[i] = c->clock;
		R3D_ACC(c->st->t_tex, t_);
	}
	return blk;
}

static uint32_t tc_tex(void *ctx, const r3d_tex *tx, uint32_t s, uint32_t t)
{
	r3d_ctx *c = (r3d_ctx *)ctx;
	const uint32_t csh = tc_csh(tx), rsh = 8u - csh;
	const uint32_t bx = s >> csh, by = t >> rsh;
	/* identidad: desplazamiento (16 bits), tamaños y formato y transparencia (10), paleta (13), bloque (12) */
	const uint64_t tag = (uint64_t)((tx->key & 0xFFFFu) | (((tx->key >> 20) & 0x3FFu) << 16)) | ((uint64_t)tx->pkey << 26) |
			     ((uint64_t)((by << 4) | bx) << 39);

	if (tag != c->last_tag) {
		c->last_blk = tc_block(c, tx, tag, bx, by);
		c->last_tag = tag;
	} else {
		c->st->tex_hits++;
	}
	return c->last_blk[((t & ((1u << rsh) - 1u)) << csh) | (s & ((1u << csh) - 1u))];
}

static void tc_reset(r3d_ctx *c)
{
	for (int i = 0; i < R3D_TC_BLOCKS; i++) {
		c->ttag[i] = ~0ull;
		c->tstamp[i] = 0;
	}
	for (int i = 0; i < R3D_LC_LINES + R3D_LP_LINES; i++)
		c->ltag[i] = 0xFFFFFFFFu;
	memset(c->lru, 0, sizeof c->lru);
	c->last_tag = ~0ull;
	c->last_blk = c->tcache[0];
	c->clock = 0;
}

/* ---- orden ----------------------------------------------------------------------------------------- */

/* Durante el orden, el segundo búfer de color (libre hasta la primera banda) guarda los trozos de polígonos y el
 * arreglo auxiliar del radix: 2 × 64 × 48 + 2048 × 4 = 14 KB de sus 18. */
typedef struct sort_scratch {
	gx_poly buf[2][R3D_SORT_CHUNK];
	uint32_t tmp[GX_MAX_POLYS];
} sort_scratch;
_Static_assert(sizeof(sort_scratch) <= sizeof(((r3d_ctx *)0)->col[1]), "el espacio del orden cabe en un búfer de color");

static void sort_polys(r3d_ctx *c, const gx_frame *hdr, uint32_t n)
{
	sort_scratch *ss = (sort_scratch *)(void *)c->col[1];
	const uint32_t manual = hdr->swap_params & 1u;
	const uint64_t pea = c->job.frame_ea + offsetof(gx_frame, poly);
	uint32_t nchunks = (n + R3D_SORT_CHUNK - 1) / R3D_SORT_CHUNK;

	if (nchunks)
		r3d_get(ss->buf[0], pea, (n < R3D_SORT_CHUNK ? n : R3D_SORT_CHUNK) * (uint32_t)sizeof(gx_poly), R3D_TAG_SORT);
	for (uint32_t k = 0; k < nchunks; k++) {
		const uint32_t b = k & 1u, i0 = k * R3D_SORT_CHUNK;
		const uint32_t cnt = n - i0 < R3D_SORT_CHUNK ? n - i0 : R3D_SORT_CHUNK;

		r3d_wait(1u << (R3D_TAG_SORT + b));
		if (k + 1 < nchunks) {
			const uint32_t i1 = i0 + R3D_SORT_CHUNK, cnt1 = n - i1 < R3D_SORT_CHUNK ? n - i1 : R3D_SORT_CHUNK;

			r3d_get(ss->buf[b ^ 1u], pea + (uint64_t)i1 * sizeof(gx_poly), cnt1 * (uint32_t)sizeof(gx_poly),
				R3D_TAG_SORT + (b ^ 1u));
		}
		for (uint32_t q = 0; q < cnt; q++) {
			const gx_poly *p = &ss->buf[b][q];

			ss->tmp[i0 + q] = (r3d_sort_key(p, manual) << 11) | (i0 + q);
			c->ylo[i0 + q] = p->ymin;
			c->yhi[i0 + q] = p->ymax;
		}
	}
	/* radix estable de la clave (17 bits) en 3 pasadas de 6, 6 y 5 bits; empieza en el orden de los índices */
	{
		uint32_t *src = ss->tmp, *dst = c->order;

		for (uint32_t pass = 0; pass < 3; pass++) {
			const uint32_t sh = 11u + pass * 6u;
			uint32_t cnt[64];

			memset(cnt, 0, sizeof cnt);
			for (uint32_t i = 0; i < n; i++)
				cnt[(src[i] >> sh) & 63u]++;
			for (uint32_t i = 0, s = 0; i < 64; i++) {
				const uint32_t v = cnt[i];

				cnt[i] = s;
				s += v;
			}
			for (uint32_t i = 0; i < n; i++)
				dst[cnt[(src[i] >> sh) & 63u]++] = src[i];
			{
				uint32_t *t = src;

				src = dst;
				dst = t;
			}
		}
		if (src != c->order)
			memcpy(c->order, src, n * sizeof(uint32_t));
	}
}

/* ---- lotes de polígonos ------------------------------------------------------------------------------ */

static void fetch_polys(r3d_ctx *c, uint32_t b, const uint16_t *idx, uint32_t m)
{
	const uint64_t pea = c->job.frame_ea + offsetof(gx_frame, poly);

	for (uint32_t i = 0; i < m; i++)
		r3d_lel_set(&c->pl[b][i], pea + (uint64_t)idx[i] * sizeof(gx_poly), (uint32_t)sizeof(gx_poly));
	r3d_getl(c->pb[b], c->job.frame_ea, c->pl[b], m, R3D_TAG_POLY + b);
	c->st->dma_bytes += m * (uint32_t)sizeof(gx_poly);
}

static void fetch_verts(r3d_ctx *c, uint32_t b, uint32_t m)
{
	const uint64_t vea = c->job.frame_ea + offsetof(gx_frame, vtx);
	uint32_t k = 0;

	for (uint32_t i = 0; i < m; i++) {
		const gx_poly *p = &c->pb[b][i];
		const uint32_t nv = p->nverts > GX_POLY_MAXV ? GX_POLY_MAXV : p->nverts;

		for (uint32_t v = 0; v < nv; v++)
			r3d_lel_set(&c->vl[b][k++], vea + (uint64_t)(p->vidx[v] % GX_MAX_VERTS) * sizeof(gx_vertex),
				    (uint32_t)sizeof(gx_vertex));
	}
	if (k)
		r3d_getl(c->vb[b], c->job.frame_ea, c->vl[b], k, R3D_TAG_VTX + b);
	c->st->dma_bytes += k * (uint32_t)sizeof(gx_vertex);
}

/* ---- una banda ---------------------------------------------------------------------------------------- */

static void draw_band(r3d_ctx *c, uint32_t n, int32_t y0, int32_t y1, uint32_t cb)
{
	const int edge = (c->rs.d3 & D3_EDGE) && !(c->rs.flags & R3D_F_NOEDGE);
	const int32_t r0 = edge && y0 > 0 ? y0 - 1 : y0;               /* primera línea dibujada */
	const int32_t r1 = edge && y1 < R3D_H ? y1 + 1 : y1;           /* una después de la última */
	const int32_t base = y0 - (edge ? 1 : 0);                      /* línea de la fila 0 de los búferes */
	uint32_t (*col)[R3D_W] = c->col[cb];
	uint32_t m = 0;
	uint32_t t_;

	/* borrado */
	t_ = R3D_T();
	for (int32_t y = r0; y < r1; y++) {
		uint32_t *cr = col[y - base], *dr = c->dep[y - base], *ar = c->att[y - base];

		for (int x = 0; x < R3D_W; x++) {
			cr[x] = c->rs.clear_px;
			dr[x] = c->rs.clear_depth;
			ar[x] = c->rs.clear_attr;
		}
	}
	/* lista de la banda, en orden de dibujo */
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t pi = c->order[i] & 2047u;

		if (c->yhi[pi] >= r0 && c->ylo[pi] < r1)
			c->list[m++] = (uint16_t)pi;
	}
	R3D_ACC(c->st->t_list, t_);
	if (m == 0)
		return;
	/* lotes: el siguiente se pide mientras se dibuja el actual */
	{
		const uint32_t nb = (m + R3D_PBATCH - 1) / R3D_PBATCH;

		t_ = R3D_T();
		fetch_polys(c, 0, c->list, m < R3D_PBATCH ? m : R3D_PBATCH);
		r3d_wait(1u << R3D_TAG_POLY);
		fetch_verts(c, 0, m < R3D_PBATCH ? m : R3D_PBATCH);
		R3D_ACC(c->st->t_fetch, t_);
		for (uint32_t k = 0; k < nb; k++) {
			const uint32_t b = k & 1u, i0 = k * R3D_PBATCH;
			const uint32_t cnt = m - i0 < R3D_PBATCH ? m - i0 : R3D_PBATCH;
			uint32_t vo = 0;

			t_ = R3D_T();
			if (k + 1 < nb) {
				const uint32_t i1 = i0 + R3D_PBATCH;

				fetch_polys(c, b ^ 1u, &c->list[i1], m - i1 < R3D_PBATCH ? m - i1 : R3D_PBATCH);
			}
			r3d_wait(1u << (R3D_TAG_VTX + b));
			R3D_ACC(c->st->t_fetch, t_);
			t_ = R3D_T();
			for (uint32_t q = 0; q < cnt; q++) {
				const gx_poly *gp = &c->pb[b][q];
				const uint32_t nv = gp->nverts > GX_POLY_MAXV ? GX_POLY_MAXV : gp->nverts;
				const gx_vertex *vv[GX_POLY_MAXV];
				r3d_poly p;

				for (uint32_t v = 0; v < nv; v++)
					vv[v] = &c->vb[b][vo + v];
				vo += nv;
				if (nv < 3 || !r3d_poly_setup(&p, gp, vv, &c->rs))
					continue;
				c->st->band_polys++;
				{
					const int32_t ya = p.ytop > r0 ? p.ytop : r0;
					const int32_t yb = p.ybot < r1 - 1 ? p.ybot : r1 - 1;

					for (int32_t y = ya; y <= yb; y++) {
						r3d_row row;

						if (!r3d_row_at(&p, y, &row))
							continue;
						c->st->rows++;
						r3d_draw_row(&p, &row, &c->rs, col[y - base], c->dep[y - base], c->att[y - base], tc_tex, c,
							     c->st);
					}
				}
			}
			R3D_ACC(c->st->t_raster, t_);
			if (k + 1 < nb) {
				const uint32_t i1 = i0 + R3D_PBATCH;

				t_ = R3D_T();
				r3d_wait(1u << (R3D_TAG_POLY + (b ^ 1u)));
				fetch_verts(c, b ^ 1u, m - i1 < R3D_PBATCH ? m - i1 : R3D_PBATCH);
				R3D_ACC(c->st->t_fetch, t_);
			}
		}
	}
}

static void post_band(r3d_ctx *c, int32_t y0, int32_t y1, uint32_t cb)
{
	const int edge = (c->rs.d3 & D3_EDGE) && !(c->rs.flags & R3D_F_NOEDGE);
	const int fog = (c->rs.d3 & D3_FOG) && !(c->rs.flags & R3D_F_NOFOG);
	const int32_t base = y0 - (edge ? 1 : 0);
	uint32_t (*col)[R3D_W] = c->col[cb];

	if (edge)
		for (int32_t y = y0; y < y1; y++) {
			const int32_t r = y - base;

			r3d_edge_row(&c->rs, col[r], c->dep[r], c->att[r], y > 0 ? c->dep[r - 1] : NULL, y > 0 ? c->att[r - 1] : NULL,
				     y < R3D_H - 1 ? c->dep[r + 1] : NULL, y < R3D_H - 1 ? c->att[r + 1] : NULL);
		}
	if (fog)
		for (int32_t y = y0; y < y1; y++)
			r3d_fog_row(&c->rs, col[y - base], c->dep[y - base], c->att[y - base]);
}

int r3d_core_render(r3d_ctx *c, const r3d_job *j, r3d_stats *st)
{
	const uint32_t t0 = R3D_T();
	uint32_t n, nbands, cb = 0;
	const gx_frame *hdr;

	memset(st, 0, sizeof *st);
	c->st = st;
	c->job = *j;
	if (j->band == 0 || j->band > R3D_BAND_MAX || (R3D_H % j->band) != 0 || j->nunits == 0 || j->unit >= j->nunits ||
	    (j->out_ea & 127u) || (j->frame_ea & 127u) || (j->vram_ea & 15u))
		return -1;
	nbands = R3D_H / j->band;
	if (j->flags & R3D_F_BLANK) {
		/* sin cuadro: transparente */
		memset(c->col[0], 0, sizeof c->col[0]);
		for (uint32_t b = j->unit; b < nbands; b += j->nunits) {
			r3d_put(c->col[0], j->out_ea + (uint64_t)b * j->band * R3D_W * 4u, j->band * R3D_W * 4u, R3D_TAG_OUT);
			st->bands++;
		}
		r3d_wait(1u << R3D_TAG_OUT);
		R3D_ACC(st->t_total, t0);
		return 0;
	}
	/* cabecera del cuadro: npolys, swap_params y la foto de los registros */
	r3d_get(c->tail, j->frame_ea + R3D_TAIL_OFF, (uint32_t)R3D_TAIL_SIZE, R3D_TAG_HDR);
	r3d_wait(1u << R3D_TAG_HDR);
	hdr = (const gx_frame *)(const void *)(c->tail - R3D_TAIL_OFF);
	n = hdr->npolys > GX_MAX_POLYS ? GX_MAX_POLYS : hdr->npolys;
	st->polys = n;
	r3d_rstate_init(&c->rs, hdr, j->flags);
	tc_reset(c);
	{
		const uint32_t t_ = R3D_T();

		sort_polys(c, hdr, n);
		R3D_ACC(st->t_sort, t_);
	}
	for (uint32_t b = j->unit; b < nbands; b += j->nunits) {
		const int32_t y0 = (int32_t)(b * j->band), y1 = y0 + (int32_t)j->band;
		const int edge = (c->rs.d3 & D3_EDGE) && !(c->rs.flags & R3D_F_NOEDGE);
		uint32_t t_;

		/* el búfer de color `cb` lo usó la salida de hace dos bandas */
		r3d_wait(1u << (R3D_TAG_OUT + cb));
		draw_band(c, n, y0, y1, cb);
		t_ = R3D_T();
		post_band(c, y0, y1, cb);
		R3D_ACC(st->t_post, t_);
		t_ = R3D_T();
		r3d_put(c->col[cb][edge ? 1 : 0], j->out_ea + (uint64_t)y0 * R3D_W * 4u, j->band * R3D_W * 4u, R3D_TAG_OUT + cb);
		st->dma_bytes += j->band * R3D_W * 4u;
		R3D_ACC(st->t_out, t_);
		st->bands++;
		cb ^= 1u;
	}
	r3d_wait((1u << R3D_TAG_OUT) | (1u << (R3D_TAG_OUT + 1)));
	R3D_ACC(st->t_total, t0);
	return 0;
}

/* ---- sondas de aritmética (autoprueba: el SPE contra el PPE) -------------------------------------------- */

static uint32_t probe_lcg;

static uint32_t prnd(void)
{
	probe_lcg = probe_lcg * 1664525u + 1013904223u;
	return probe_lcg;
}

uint32_t r3d_math_probe(uint32_t seed, uint32_t which)
{
	uint64_t h = 0xcbf29ce484222325ull;

	probe_lcg = seed;
	for (int i = 0; i < 4096; i++) {
		uint64_t v = 0;

		switch (which) {
		case 0: { /* factor de interpolación */
			const uint32_t len = 1 + (prnd() & 511), pos = prnd() % (len + 1), w0 = 1 + (prnd() & 0xFFFF), w1 = 1 + (prnd() & 0xFFFF);

			v = r3d_pfac(pos, len, w0, w1, 8 + (prnd() & 1));
			break;
		}
		case 1: { /* lerp con signo */
			const int32_t a = (int32_t)(prnd() & 0xFFFFFF) - 0x800000, b = (int32_t)(prnd() & 0xFFFFFF) - 0x800000;

			v = (uint32_t)r3d_lerp(a, b, prnd() & 511, 9);
			break;
		}
		case 2: { /* x de arista en punto fijo */
			r3d_v a, b;

			a.x = (int32_t)(prnd() & 1023) - 512;
			a.y = (int32_t)(prnd() & 255) - 32;
			b.x = (int32_t)(prnd() & 1023) - 512;
			b.y = a.y + 1 + (int32_t)(prnd() & 255);
			v = (uint64_t)(int64_t)r3d_fx(r3d_xfix(&a, &b, a.y + (int32_t)(prnd() % (uint32_t)(b.y - a.y))));
			break;
		}
		case 3: { /* división de 64 bits con signo (la z lineal) */
			const int64_t zl = prnd() & 0xFFFFFF, zr = prnd() & 0xFFFFFF;
			const int64_t L = 1 + (prnd() & 511), k = prnd() % (uint32_t)L;

			v = (uint64_t)(zl + (zr - zl) * k / L);
			break;
		}
		case 4: { /* desplazamiento aritmético de 64 bits por una cantidad variable, positivos */
			const int64_t x = (int64_t)(prnd() & 0x7FFFFFFFu) * 1048576 + (int64_t)prnd();

			v = (uint64_t)(x >> (prnd() & 31));
			break;
		}
		case 5: { /* ídem, negativos */
			const int64_t x = -((int64_t)(prnd() & 0x7FFFFFFFu) * 1048576 + (int64_t)prnd());

			v = (uint64_t)(x >> (prnd() & 31));
			break;
		}
		case 6: { /* desplazamiento a la izquierda de 64 bits sin signo, variable */
			const uint64_t x = prnd() & 0xFFFFFFu;

			v = x << (prnd() & 15);
			break;
		}
		case 7: { /* multiplicación de 64 bits */
			const int64_t x = (int32_t)prnd(), y = (int32_t)prnd();

			v = (uint64_t)(x * y);
			break;
		}
		default: { /* una línea entera de un triángulo con color (r3d_row_at + r3d_draw_row, sin textura) */
			static uint32_t col[R3D_W], dep[R3D_W], att[R3D_W];
			r3d_rstate rs;
			r3d_poly p;
			r3d_row row;
			r3d_stats st;
			int32_t y;

			if (i >= 256)
				break;
			memset(&rs, 0, sizeof rs);
			memset(&p, 0, sizeof p);
			rs.d3 = D3_BLEND;
			rs.wbuf = prnd() & 1;
			p.n = 3;
			p.alpha = 1 + (prnd() % 31);
			p.id = prnd() & 63;
			p.attr = (p.alpha << 16) | (p.id << 24);
			p.ytop = 0x7FFFFFFF;
			p.ybot = -0x7FFFFFFF;
			for (int k = 0; k < 3; k++) {
				r3d_v *d = &p.v[k];

				d->x = (int32_t)(prnd() % 300) - 20;
				d->y = (int32_t)(prnd() % 200) - 4;
				d->z = prnd() & 0xFFFFFF;
				d->w = 1 + (prnd() & 0xFFFF);
				d->r = (int32_t)((prnd() & 63) << 3 | 7);
				d->g = (int32_t)((prnd() & 63) << 3 | 7);
				d->b = (int32_t)((prnd() & 63) << 3 | 7);
				if (d->y < p.ytop) {
					p.ytop = d->y;
					p.top = k;
				}
				if (d->y > p.ybot)
					p.ybot = d->y;
			}
			p.ldir = (int64_t)(p.v[1].x - p.v[0].x) * (p.v[2].y - p.v[0].y) - (int64_t)(p.v[2].x - p.v[0].x) * (p.v[1].y - p.v[0].y) > 0 ? -1 : 1;
			y = p.ytop + (int32_t)(prnd() % (uint32_t)(p.ybot - p.ytop + 1));
			for (int x = 0; x < R3D_W; x++) {
				col[x] = prnd();
				dep[x] = prnd() & 0xFFFFFF;
				att[x] = prnd() & 0xFFFF;
			}
			memset(&row, 0, sizeof row);
			memset(&st, 0, sizeof st);
			if (r3d_row_at(&p, y, &row)) {
				r3d_draw_row(&p, &row, &rs, col, dep, att, NULL, NULL, &st);
				for (int x = 0; x < R3D_W; x++)
					h = (h ^ col[x] ^ ((uint64_t)dep[x] << 32) ^ ((uint64_t)att[x] << 7)) * 0x100000001b3ull;
			}
			v = (uint64_t)row.xl << 32 | (uint32_t)row.xr;
			break;
		}
		}
		h = (h ^ v) * 0x100000001b3ull;
	}
	return (uint32_t)(h ^ (h >> 32));
}
