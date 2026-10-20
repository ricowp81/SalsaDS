/* SPDX-License-Identifier: GPL-2.0-or-later */
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
 *      demanda: el bloque se decodifica por tramos de la fila (tc_fill: la VRAM entra por una caché de líneas de 128
 *      bytes y la paleta ya convertida se queda en el local store), con los mismos valores que r3d_decode. Se vacía al
 *      empezar el cuadro (la VRAM pudo cambiar) y dura todas las bandas del SPE: lo que desbordaba la caché de texturas
 *      enteras de la Fase 0.4 (cada banda recorría todas las texturas) ya no pasa, porque solo se decodifica lo que se
 *      muestrea.
 *   4. los píxeles de cada línea de polígono los dibuja r3d_vrow.h en vectores de 4 (Fase 3.4); r3d_draw_row de r3d_px.h
 *      queda como referencia (r3d_ref.c) y da los mismos bits.
 *
 * Presupuesto del local store del contexto (sizeof(r3d_ctx), lo imprime el SPE; programa en src/spu/raster3d):
 *   color 2 × 18 líneas × 1 KB = 36 KB (doble búfer de salida), profundidad 18 KB, atributos 18 KB,
 *   orden y extensiones 2048 × (4 + 4 + 2 + 2) = 24 KB, lista de la banda 4 KB, lotes 2 × (16 × 48 + 160 × 32) = 11,5 KB,
 *   listas de DMA 2 × 160 × 8 = 2,5 KB, caché de texturas 64 KB + etiquetas 1 KB, líneas de VRAM 40 × 128 = 5 KB,
 *   paleta convertida 1 KB y tramo de VRAM 0,5 KB.
 *   ≈ 178,4 KB (182 656 B); el programa (≈ 55 KB) y la pila van en el resto (ver la cabecera de src/spu/raster3d/raster3d.c).
 */
#include <stddef.h>
#include <string.h>

#include "gpu3d/r3d.h"
#include "gpu3d/r3d_core.h"
#include "gpu3d/r3d_px.h"

/* Código que no está en el camino caliente (se corre una vez por bloque o por cuadro): compilado para tamaño, que el local
 * store del SPE está justo. */
#if defined(__SPU__)
#define R3D_SMALL __attribute__((optimize("Os")))
#else
#define R3D_SMALL
#endif

#ifndef __SPU__
uint32_t r3d_io_errors;
#endif
#ifdef R3D_VERIFY_DECODE
uint32_t r3d_decode_errors; /* texeles del bloque decodificado que difieren de r3d_decode */
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

/* El byte `a` de la memoria de texturas (espacio 0, 512 KB) o de paletas (1, 128 KB) en el local store. El puntero vale
 * hasta la siguiente búsqueda en el mismo espacio. */
static __attribute__((noinline)) const uint8_t *lc_line(r3d_ctx *c, uint32_t space, uint32_t a)
{
	/* asociativas de 2 vías (conjunto = línea & n/2 - 1; se reemplaza la que no se usó por última vez), una para
	 * texturas y otra para paletas: los datos, los índices del 4x4 y la paleta de un mismo texel no se pisan */
	const uint32_t key = a >> 7;
	const uint32_t base = space ? R3D_LC_LINES : 0u, half = space ? R3D_LP_LINES / 2u : R3D_LC_LINES / 2u;
	const uint32_t i0 = base + (key & (half - 1u)), i1 = i0 + half;
	uint32_t i;

	if (c->ltag[i0] == key) {
		c->lru[i0 - base] = 0;
		return &c->lines[i0][a & 127u];
	}
	if (c->ltag[i1] == key) {
		c->lru[i0 - base] = 1;
		return &c->lines[i1][a & 127u];
	}
	i = c->lru[i0 - base] ? i0 : i1; /* la que no se usó */
	c->lru[i0 - base] = (uint8_t)(i == i1);
	r3d_get(c->lines[i], vram_line_ea(c, space, a), 128, R3D_TAG_LINE);
	r3d_wait(1u << R3D_TAG_LINE);
	c->ltag[i] = key;
	c->st->line_miss++;
	if (space)
		c->st->pal_miss++;
	return &c->lines[i][a & 127u];
}

static inline uint32_t lc_rd(r3d_ctx *c, uint32_t space, uint32_t a)
{
	return *lc_line(c, space, a);
}

/* Copia `len` bytes seguidos de la memoria de texturas o paletas (con la vuelta de dirección de cada una) al local store. */
static void lc_copy(r3d_ctx *c, uint32_t space, uint32_t a, uint32_t len, uint8_t *dst)
{
	const uint32_t mask = space ? 0x1FFFFu : 0x7FFFFu;

	while (len) {
		uint32_t n = 128u - (a & 127u);

		if (n > len)
			n = len;
		memcpy(dst, lc_line(c, space, a & mask), n);
		dst += n;
		a += n;
		len -= n;
	}
}

static __attribute__((unused)) uint32_t lc_rdt(void *ctx, uint32_t a)
{
	return lc_rd((r3d_ctx *)ctx, 0, a & 0x7FFFFu);
}

static __attribute__((unused)) uint32_t lc_rdp(void *ctx, uint32_t a)
{
	return lc_rd((r3d_ctx *)ctx, 1, a & 0x1FFFFu);
}

/* ---- líneas de 1 KB (modo de celdas, 3.9) ----------------------------------------------------------- */

/* Dirección de la línea de R3D_CL_BYTES que contiene el byte `a` (ya acotado) de la memoria de texturas (espacio 0, 512 KB)
 * o de paletas (1, 128 KB) en la instantánea de la VRAM; 0 si el banco no está mapeado (se lee cero). */
static uint64_t cl_line_ea(const r3d_ctx *c, uint32_t space, uint32_t a)
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
		return 0;
	return c->job.vram_ea + off + (a & ~(uint32_t)(R3D_CL_BYTES - 1u));
}

/* Puntero al byte `a` (acotado) de la memoria de texturas (espacio 0) o de paletas (1), en una de las líneas de 1 KB del local
 * store (asociativas de 4 vías, la que lleva más tiempo sin usarse se reemplaza). Cada memoria recuerda su última línea. El puntero
 * vale hasta la siguiente búsqueda en el mismo espacio. */
static const uint8_t *cl_line(r3d_ctx *c, uint32_t space, uint32_t a)
{
	const uint32_t key = a >> R3D_CL_SHIFT, off = a & (R3D_CL_BYTES - 1u);
	uint32_t *tags = space ? c->pltag : c->dltag, *stamps = space ? c->plstamp : c->dlstamp;
	const uint32_t nsets = space ? R3D_CL_PLINES / R3D_CL_WAYS : R3D_CL_DLINES / R3D_CL_WAYS;
	const uint32_t base = (key & (nsets - 1u)) * R3D_CL_WAYS;
	uint32_t victim = base, oldest = 0xFFFFFFFFu;
	uint8_t *line;
	uint64_t ea;

	if (space == 0 ? c->dl_key == key : c->pl_key == key)
		return (space ? c->pl_ptr : c->dl_ptr) + off;
	c->clock++;
	for (uint32_t w = 0; w < R3D_CL_WAYS; w++) {
		const uint32_t i = base + w;

		if (tags[i] == key) {
			stamps[i] = c->clock;
			line = space ? c->pline[i] : c->dline[i];
			goto found;
		}
		if (stamps[i] < oldest) {
			oldest = stamps[i];
			victim = i;
		}
	}
	line = space ? c->pline[victim] : c->dline[victim];
	ea = cl_line_ea(c, space, a);
	if (ea) {
		r3d_get(line, ea, R3D_CL_BYTES, R3D_TAG_LINE);
		r3d_wait(1u << R3D_TAG_LINE);
	} else {
		memset(line, 0, R3D_CL_BYTES);
	}
	tags[victim] = key;
	stamps[victim] = c->clock;
	c->st->line_miss++;
	if (space)
		c->st->pal_miss++;
found:
	if (space) {
		c->pl_key = key;
		c->pl_ptr = line;
	} else {
		c->dl_key = key;
		c->dl_ptr = line;
	}
	return line + off;
}

static inline const uint8_t *tx_line(r3d_ctx *c, uint32_t space, uint32_t a)
{
	return c->cellmode ? cl_line(c, space, a) : lc_line(c, space, a);
}

/* ---- caché de bloques decodificados ------------------------------------------------------------------ */

static inline uint32_t tc_csh(const r3d_tex *tx)
{
	return tx->sl < 6 ? tx->sl : 6u;
}

/* Paleta ya convertida (r3d_555 con alfa 0) de `n` entradas a partir de tx->pal; dura todo el cuadro. */
static const uint32_t *tc_pal(r3d_ctx *c, const r3d_tex *tx, uint32_t n)
{
	if (c->pal_n < n || c->pal_addr != tx->pal) {
		lc_copy(c, 1, tx->pal, n * 2u, c->stage);
		for (uint32_t i = 0; i < n; i++)
			c->pxtab[i] = r3d_555((uint32_t)c->stage[2u * i] | ((uint32_t)c->stage[2u * i + 1u] << 8), 0);
		c->pal_addr = tx->pal;
		c->pal_n = n;
	}
	return c->pxtab;
}

/* Texeles comprimidos 4x4 (formato 5): por cada tile, los índices de 2 bits, la palabra de modo y de paleta y los
 * colores; lo mismo que r3d_decode, un tile a la vez. Rellena `rows` filas de `cols` texeles desde (s0, t0). */
static R3D_SMALL void tc_fill5(r3d_ctx *c, const r3d_tex *tx, uint32_t *blk, uint32_t s0, uint32_t t0, uint32_t cols, uint32_t nr,
		     uint32_t csh)
{
	const uint32_t S = 1u << tx->sl, tw = S >> 2;

	for (uint32_t ty = 0; ty < nr; ty += 4) {
		for (uint32_t tq = 0; tq < cols; tq += 4) {
			const uint32_t blkn = ((t0 + ty) >> 2) * tw + ((s0 + tq) >> 2);
			const uint32_t da = tx->addr + blkn * 4u;
			const uint32_t ia = 0x20000u + ((da & 0x1FFFFu) >> 1) + (((da >> 18) & 1u) ? 0x10000u : 0u);
			uint8_t dat[4];
			uint32_t pi, mode, pa, px[4];
			const uint8_t *q;

			memcpy(dat, tx_line(c, 0, da & 0x7FFFFu), 4);
			q = tx_line(c, 0, ia & 0x7FFFFu);
			pi = (uint32_t)q[0] | ((uint32_t)q[1] << 8);
			mode = pi >> 14;
			pa = tx->pal + (pi & 0x3FFFu) * 4u;
			{
				uint8_t pb[8];
				uint32_t c0, c1;

				memcpy(pb, tx_line(c, 1, pa & 0x1FFFFu), 4);
				if (mode == 0 || mode == 2)
					memcpy(pb + 4, tx_line(c, 1, (pa + 4u) & 0x1FFFFu), 4);
				c0 = (uint32_t)pb[0] | ((uint32_t)pb[1] << 8);
				c1 = (uint32_t)pb[2] | ((uint32_t)pb[3] << 8);
				px[0] = r3d_555(c0, 31);
				px[1] = r3d_555(c1, 31);
				if (mode == 0 || mode == 2) {
					px[2] = r3d_555((uint32_t)pb[4] | ((uint32_t)pb[5] << 8), 31);
					px[3] = mode == 0 ? 0u : r3d_555((uint32_t)pb[6] | ((uint32_t)pb[7] << 8), 31);
				} else {
					uint32_t m2 = 0, m3 = 0;

					for (uint32_t k = 0; k < 15; k += 5) {
						const uint32_t a = (c0 >> k) & 31u, b = (c1 >> k) & 31u;

						if (mode == 1) {
							m2 |= ((a + b) >> 1) << k;
						} else {
							m2 |= ((a * 5u + b * 3u) >> 3) << k;
							m3 |= ((a * 3u + b * 5u) >> 3) << k;
						}
					}
					px[2] = r3d_555(m2, 31);
					px[3] = mode == 1 ? 0u : r3d_555(m3, 31);
				}
			}
			for (uint32_t r = 0; r < 4; r++) {
				const uint32_t b = dat[r];
				uint32_t *o = blk + ((ty + r) << csh) + tq;

				o[0] = px[b & 3u];
				o[1] = px[(b >> 2) & 3u];
				o[2] = px[(b >> 4) & 3u];
				o[3] = px[b >> 6];
			}
		}
	}
}

/* Decodifica el bloque (bx, by) de la textura en blk (cols × filas de texeles R3D_PX, filas de 1 << csh palabras) con los
 * mismos valores que r3d_decode, leyendo la VRAM por tramos y no texel a texel. */
static R3D_SMALL void tc_fill(r3d_ctx *c, const r3d_tex *tx, uint32_t *blk, uint32_t bx, uint32_t by)
{
	const uint32_t csh = tc_csh(tx), rsh = 8u - csh;
	const uint32_t cols = 1u << csh, rows = 1u << rsh;
	const uint32_t S = 1u << tx->sl, T = 1u << tx->tl;
	const uint32_t t0 = by << rsh, s0 = bx << csh;
	const uint32_t nr = t0 + rows > T ? T - t0 : rows;
	const uint32_t fmt = tx->fmt;
	/* bits por texel y entradas de la paleta */
	static const uint8_t bpp[8] = { 0, 8, 2, 4, 8, 0, 8, 16 };
	static const uint16_t npal[8] = { 0, 32, 4, 16, 256, 0, 8, 0 };

	if (fmt == 5) {
		tc_fill5(c, tx, blk, s0, t0, cols, nr, csh);
		return;
	}
	{
		const uint32_t *pt = npal[fmt] ? tc_pal(c, tx, npal[fmt]) : NULL;
		const uint32_t b = bpp[fmt];
		/* si el bloque cubre filas enteras de la textura (cols == S) es un solo tramo; si no, un tramo por fila */
		const uint32_t nseg = cols == S ? 1u : nr;
		const uint32_t cnt = cols == S ? nr * cols : cols;

		for (uint32_t sg = 0; sg < nseg; sg++) {
			const uint32_t i0 = (cols == S ? t0 * S : (t0 + sg) * S + s0);
			uint32_t *o = blk + (cols == S ? 0u : (sg << csh));
			const uint8_t *src = c->stage;

			lc_copy(c, 0, tx->addr + ((i0 * b) >> 3), (cnt * b) >> 3, c->stage);
			switch (fmt) {
			case 1: /* A3I5 */
				for (uint32_t j = 0; j < cnt; j++) {
					const uint32_t v = src[j], a3 = v >> 5;

					o[j] = pt[v & 31u] | (((a3 << 2) + (a3 >> 1)) << 24);
				}
				break;
			case 2: /* 4 colores */
				for (uint32_t j = 0; j < cnt; j += 4) {
					const uint32_t v = src[j >> 2];

					for (uint32_t k = 0; k < 4; k++) {
						const uint32_t idx = (v >> (2u * k)) & 3u;

						o[j + k] = (idx == 0 && tx->c0t) ? 0u : (pt[idx] | (31u << 24));
					}
				}
				break;
			case 3: /* 16 colores */
				for (uint32_t j = 0; j < cnt; j += 2) {
					const uint32_t v = src[j >> 1], i0_ = v & 15u, i1_ = v >> 4;

					o[j] = (i0_ == 0 && tx->c0t) ? 0u : (pt[i0_] | (31u << 24));
					o[j + 1] = (i1_ == 0 && tx->c0t) ? 0u : (pt[i1_] | (31u << 24));
				}
				break;
			case 4: /* 256 colores */
				for (uint32_t j = 0; j < cnt; j++) {
					const uint32_t v = src[j];

					o[j] = (v == 0 && tx->c0t) ? 0u : (pt[v] | (31u << 24));
				}
				break;
			case 6: /* A5I3 */
				for (uint32_t j = 0; j < cnt; j++) {
					const uint32_t v = src[j];

					o[j] = pt[v & 7u] | ((v >> 3) << 24);
				}
				break;
			default: /* 7: directo */
				for (uint32_t j = 0; j < cnt; j++) {
					const uint32_t v = (uint32_t)src[2u * j] | ((uint32_t)src[2u * j + 1u] << 8);

					o[j] = r3d_555(v & 0x7FFFu, (v & 0x8000u) ? 31u : 0u);
				}
				break;
			}
		}
	}
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
		const uint32_t t_ = R3D_T();

		if (c->ttag[i] != ~0ull)
			c->st->tex_evict++;
		c->st->tex_misses++;
		blk = c->tcache[i];
		tc_fill(c, tx, blk, bx, by);
#ifdef R3D_VERIFY_DECODE
		{ /* contra r3d_decode texel a texel (pruebas del host) */
			const uint32_t csh = tc_csh(tx), rsh = 8u - csh, cols = 1u << csh, rows = 1u << rsh, T = 1u << tx->tl;
			const uint32_t t0 = by << rsh, s0 = bx << csh, nr = t0 + rows > T ? T - t0 : rows;

			for (uint32_t r = 0; r < nr; r++)
				for (uint32_t q = 0; q < cols; q++)
					if (blk[(r << csh) | q] != r3d_decode(tx, s0 + q, t0 + r, lc_rdt, lc_rdp, c))
						r3d_decode_errors++;
		}
#endif
		c->ttag[i] = tag;
		c->tstamp[i] = c->clock;
		R3D_ACC(c->st->t_tex, t_);
	}
	return blk;
}

static uint32_t tc_tid(r3d_ctx *c, uint64_t base);

/* La textura de un polígono: su identidad para la caché y, si no es la del último bloque usado, se olvida ese bloque. */
static inline void tc_bind(r3d_ctx *c, const r3d_tex *tx)
{
	/* identidad: desplazamiento (16 bits), tamaños y formato y transparencia (10), paleta (13); el bloque va aparte */
	c->cur_base = (uint64_t)((tx->key & 0xFFFFu) | (((tx->key >> 20) & 0x3FFu) << 16)) | ((uint64_t)tx->pkey << 26);
	if (c->cur_base != c->last_base) {
		c->last_base = c->cur_base;
		c->last_id = 0xFFFFFFFFu;
		c->last_tag = ~0ull;
		if (c->cellmode) {
			c->tid = tc_tid(c, c->cur_base);
			c->last_cid = 0xFFFFFFFFu;
		}
	}
}

/* El texel `ix` del bloque `id` (by << 4 | bx) de la textura vigente (tc_bind) */
static uint32_t tc_get(r3d_ctx *c, const r3d_tex *tx, uint32_t id, uint32_t ix)
{
	const uint64_t tag = c->cur_base | ((uint64_t)id << 39);

	if (tag != c->last_tag) {
		c->last_blk = tc_block(c, tx, tag, id & 15u, id >> 4);
		c->last_tag = tag;
		c->last_id = id;
	} else {
		c->st->tex_hits++;
	}
	return c->last_blk[ix & 255u];
}

/* ---- celdas de 4×4 (3.9) ------------------------------------------------------------------------------------- */

/* Los bloques de 256 texeles decodificados enteros (3.2 a 3.8) tienen un problema en Mario Kart DS: un cuadro toca unos 620
 * bloques distintos pero la caché guarda 64, así que los mismos bloques se decodifican de nuevo en cada banda (3200 decodificados
 * por cuadro, 5 veces los distintos), y como los polígonos lejanos usan 20 de los 256 texeles de su bloque, la mayor parte del
 * trabajo es de texeles que nadie pide. Aquí la unidad es la celda de 4×4 texeles (el tile del formato 4x4): 512 celdas de 64
 * bytes caben en la mitad del espacio, y los datos de VRAM entran por líneas de 1 KB (cl_line), no de 128 bytes: el acceso a los
 * datos de una textura reducida es disperso pero local a escala de kilobytes (una textura de 64×64 comprimida son 1,5 KB). Mismos
 * valores que el bloque entero y que r3d_decode. */

#ifdef R3D_VERIFY_DECODE
static uint32_t vf_rdt(void *ctx, uint32_t a)
{
	const r3d_ctx *c = ctx;
	const uint32_t off = c->job.texslot[(a >> 17) & 3u];

	a &= 0x7FFFFu;
	return off == R3D_UNMAPPED ? 0u : ((const uint8_t *)(uintptr_t)c->job.vram_ea)[off + (a & 0x1FFFFu)];
}

static uint32_t vf_rdp(void *ctx, uint32_t a)
{
	const r3d_ctx *c = ctx;
	const uint32_t slot = (a >> 14) & 7u, off = slot < R3D_PAL_SLOTS ? c->job.palslot[slot] : R3D_UNMAPPED;

	return off == R3D_UNMAPPED ? 0u : ((const uint8_t *)(uintptr_t)c->job.vram_ea)[off + (a & 0x3FFFu)];
}
#endif

/* El color (16 bits) de la entrada `idx` de la paleta del texel, por las líneas de paletas */
static inline uint32_t cl_pe(r3d_ctx *c, const r3d_tex *tx, uint32_t idx)
{
	const uint8_t *p = cl_line(c, 1, (tx->pal + idx * 2u) & 0x1FFFFu);

	return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

/* Decodifica la celda de 4×4 texeles cuya esquina es (s0, t0) (múltiplos de 4) en `cell` (16 palabras, filas de 4) */
static R3D_SMALL void tc_cell_fill(r3d_ctx *c, const r3d_tex *tx, uint32_t *cell, uint32_t s0, uint32_t t0)
{
	const uint32_t S = 1u << tx->sl, fmt = tx->fmt;

	if (fmt == 5) {
		tc_fill5(c, tx, cell, s0, t0, 4, 4, 2);
		return;
	}
	for (uint32_t r = 0; r < 4; r++) {
		const uint32_t i = (t0 + r) * S + s0;
		uint32_t *o = cell + 4u * r;
		const uint8_t *p;

		switch (fmt) {
		case 1: /* A3I5 */
			p = cl_line(c, 0, (tx->addr + i) & 0x7FFFFu);
			for (uint32_t j = 0; j < 4; j++) {
				const uint32_t b = p[j], a3 = b >> 5;

				o[j] = r3d_555(cl_pe(c, tx, b & 31u), (a3 << 2) + (a3 >> 1));
			}
			break;
		case 2: { /* 4 colores */
			const uint32_t v = *cl_line(c, 0, (tx->addr + (i >> 2)) & 0x7FFFFu);

			for (uint32_t j = 0; j < 4; j++) {
				const uint32_t idx = (v >> (2u * j)) & 3u;

				o[j] = (idx == 0 && tx->c0t) ? 0u : r3d_555(cl_pe(c, tx, idx), 31);
			}
			break;
		}
		case 3: /* 16 colores */
			p = cl_line(c, 0, (tx->addr + (i >> 1)) & 0x7FFFFu);
			for (uint32_t j = 0; j < 4; j++) {
				const uint32_t idx = (p[j >> 1] >> ((j & 1u) * 4u)) & 15u;

				o[j] = (idx == 0 && tx->c0t) ? 0u : r3d_555(cl_pe(c, tx, idx), 31);
			}
			break;
		case 4: /* 256 colores */
			p = cl_line(c, 0, (tx->addr + i) & 0x7FFFFu);
			for (uint32_t j = 0; j < 4; j++) {
				const uint32_t idx = p[j];

				o[j] = (idx == 0 && tx->c0t) ? 0u : r3d_555(cl_pe(c, tx, idx), 31);
			}
			break;
		case 6: /* A5I3 */
			p = cl_line(c, 0, (tx->addr + i) & 0x7FFFFu);
			for (uint32_t j = 0; j < 4; j++) {
				const uint32_t b = p[j];

				o[j] = r3d_555(cl_pe(c, tx, b & 7u), b >> 3);
			}
			break;
		default: /* 7: directo */
			p = cl_line(c, 0, (tx->addr + i * 2u) & 0x7FFFFu);
			for (uint32_t j = 0; j < 4; j++) {
				const uint32_t v = (uint32_t)p[2u * j] | ((uint32_t)p[2u * j + 1u] << 8);

				o[j] = r3d_555(v & 0x7FFFu, (v & 0x8000u) ? 31u : 0u);
			}
			break;
		}
	}
}

/* Identidad pequeña (0..R3D_TID_MAX-1) de la textura `base` en este cuadro. Con más texturas que eso se vacía la caché de
 * celdas y se empieza otra vez (no pasa: los cuadros de los tres juegos de prueba usan menos de 150). */
static uint32_t tc_tid(r3d_ctx *c, uint64_t base)
{
	uint32_t h = (uint32_t)((base * 0x9E3779B97F4A7C15ull) >> 55) & 511u;

	for (;;) {
		const uint32_t e = c->tidhash[h];

		if (e == 0)
			break;
		if (c->tidbase[e - 1u] == base)
			return e - 1u;
		h = (h + 1u) & 511u;
	}
	if (c->ntid == R3D_TID_MAX) {
		for (uint32_t i = 0; i < R3D_CELLS; i++)
			c->ctag[i] = 0xFFFFFFFFu;
		memset(c->tidhash, 0, sizeof c->tidhash);
		c->ntid = 0;
		h = (uint32_t)((base * 0x9E3779B97F4A7C15ull) >> 55) & 511u;
	}
	c->tidbase[c->ntid] = base;
	c->tidhash[h] = (uint16_t)(c->ntid + 1u);
	return c->ntid++;
}

static const uint32_t *tc_cell(r3d_ctx *c, const r3d_tex *tx, uint32_t cx, uint32_t cy);

static void tc_reset(r3d_ctx *c)
{
	if (c->cellmode) {
		for (uint32_t i = 0; i < R3D_CELLS; i++) {
			c->ctag[i] = 0xFFFFFFFFu;
			c->cstamp[i] = 0;
		}
		for (uint32_t i = 0; i < R3D_CL_DLINES; i++) {
			c->dltag[i] = 0xFFFFFFFFu;
			c->dlstamp[i] = 0;
		}
		for (uint32_t i = 0; i < R3D_CL_PLINES; i++) {
			c->pltag[i] = 0xFFFFFFFFu;
			c->plstamp[i] = 0;
		}
		memset(c->tidhash, 0, sizeof c->tidhash);
		c->ntid = c->tid = 0;
		c->dl_key = c->pl_key = 0xFFFFFFFFu;
		c->last_cid = 0xFFFFFFFFu;
	} else {
		for (int i = 0; i < R3D_TC_BLOCKS; i++) {
			c->ttag[i] = ~0ull;
			c->tstamp[i] = 0;
		}
		for (int i = 0; i < R3D_LC_LINES + R3D_LP_LINES; i++)
			c->ltag[i] = 0xFFFFFFFFu;
		memset(c->lru, 0, sizeof c->lru);
	}
	c->last_tag = ~0ull;
	c->last_base = ~0ull;
	c->last_id = 0xFFFFFFFFu;
	c->last_blk = c->cellmode ? c->cell[0] : c->tcache[0];
	c->last_cell = c->cell[0];
	c->pal_n = 0;
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

/* ---- la línea de un polígono, en vectores ------------------------------------------------------------------- */

#include "gpu3d/r3d_vrow.h"

/* La celda (cx, cy) de la textura vigente (tc_bind): la busca en la caché (4 vías: las cuatro etiquetas de un conjunto son una sola
 * carga de 16 bytes; la menos reciente se reemplaza) o la decodifica. */
static const uint32_t *tc_cell(r3d_ctx *c, const r3d_tex *tx, uint32_t cx, uint32_t cy)
{
	const uint32_t tag = (c->tid << 16) | (cy << 8) | cx;
	/* conjunto: las celdas vecinas de una fila caen en conjuntos vecinos; la textura y la fila los mezclan */
	const uint32_t set = (cx + (cy << 3) + (cy << 1) + cy + (c->tid << 5) + (c->tid << 2) + c->tid) & (R3D_CELL_SETS - 1u);
	const uint32_t base = set * R3D_CELL_WAYS;
	const v4u eq = veq(*(const v4u_ma *)(const void *)&c->ctag[base], vs(tag));
	uint32_t victim = base, oldest = 0xFFFFFFFFu;

	c->clock++;
	if (vany(eq)) {
		const uint32_t i = base + (eq[0] ? 0u : eq[1] ? 1u : eq[2] ? 2u : 3u);

		c->cstamp[i] = c->clock;
		return c->cell[i];
	}
	for (uint32_t w = 0; w < R3D_CELL_WAYS; w++) {
		if (c->cstamp[base + w] < oldest) {
			oldest = c->cstamp[base + w];
			victim = base + w;
		}
	}
	{
		const uint32_t t_ = R3D_T();

		if (c->ctag[victim] != 0xFFFFFFFFu)
			c->st->tex_evict++;
		c->st->tex_misses++;
		tc_cell_fill(c, tx, c->cell[victim], cx * 4u, cy * 4u);
#ifdef R3D_VERIFY_DECODE
		for (uint32_t k = 0; k < 16; k++)
			if (c->cell[victim][k] != r3d_decode(tx, cx * 4u + (k & 3u), cy * 4u + (k >> 2), vf_rdt, vf_rdp, c))
				r3d_decode_errors++;
#endif
		c->ctag[victim] = tag;
		c->cstamp[victim] = c->clock;
		R3D_ACC(c->st->t_tex, t_);
	}
	return c->cell[victim];
}

/* Camino lento de la textura de una cuatro píxeles: algún carril fuera de la celda (bloque) del texel anterior. Compartido por las
 * variantes de la línea para no repetirlo en cada una (el local store del SPE está justo). */
static __attribute__((noinline)) v4u vr_tex_cell_slow(r3d_ctx *c, const r3d_tex *tx, v4u sv, v4u tv, v4u M)
{
	const v4u cid = ((tv >> 2) << 8) | (sv >> 2), cix = ((tv & vs(3u)) << 2) | (sv & vs(3u));
	uint32_t t[4] = { 0, 0, 0, 0 };

	for (int k = 0; k < 4; k++) {
		if (!M[k])
			continue;
		if (cid[k] != c->last_cid) {
			c->last_cell = tc_cell(c, tx, sv[k] >> 2, tv[k] >> 2);
			c->last_cid = cid[k];
		}
		t[k] = c->last_cell[cix[k]];
	}
	return (v4u){ t[0], t[1], t[2], t[3] };
}

static __attribute__((noinline)) v4u vr_tex_block_slow(r3d_ctx *c, const r3d_tex *tx, v4u id, v4u ix, v4u M)
{
	uint32_t t[4] = { 0, 0, 0, 0 };

	for (int k = 0; k < 4; k++)
		if (M[k])
			t[k] = tc_get(c, tx, id[k], ix[k]);
	return (v4u){ t[0], t[1], t[2], t[3] };
}

static __attribute__((noinline)) void vrow_lin_tex(r3d_ctx *c, const r3d_poly *p, const r3d_row *row, uint32_t *col, uint32_t *dep,
						   uint32_t *att)
{
	r3d_vrow(c, &c->rs, c->st, p, row, col, dep, att, 1, 1);
}
static __attribute__((noinline)) void vrow_persp_tex(r3d_ctx *c, const r3d_poly *p, const r3d_row *row, uint32_t *col, uint32_t *dep,
						     uint32_t *att)
{
	r3d_vrow(c, &c->rs, c->st, p, row, col, dep, att, 0, 1);
}
/* Sin textura (también lo usan la sonda de la autoprueba y las pruebas unitarias: no necesita contexto) */
void r3d_core_row_notex(const r3d_rstate *rs, r3d_stats *st, const r3d_poly *p, const r3d_row *row, uint32_t *col, uint32_t *dep,
			uint32_t *att)
{
	if (row->l.w == row->r.w)
		r3d_vrow(NULL, rs, st, p, row, col, dep, att, 1, 0);
	else
		r3d_vrow(NULL, rs, st, p, row, col, dep, att, 0, 0);
}

static inline void draw_poly_row(r3d_ctx *c, const r3d_poly *p, const r3d_row *row, uint32_t *col, uint32_t *dep, uint32_t *att)
{
	const int lin = row->l.w == row->r.w;

	if (p->textured) {
		if (lin)
			vrow_lin_tex(c, p, row, col, dep, att);
		else
			vrow_persp_tex(c, p, row, col, dep, att);
	} else {
		r3d_core_row_notex(&c->rs, c->st, p, row, col, dep, att);
	}
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

		const v4u vc = vs(c->rs.clear_px), vd = vs(c->rs.clear_depth), va = vs(c->rs.clear_attr);

		for (int x = 0; x < R3D_W; x += 4) {
			*(v4u_ma *)(void *)(cr + x) = vc;
			*(v4u_ma *)(void *)(dr + x) = vd;
			*(v4u_ma *)(void *)(ar + x) = va;
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
				uint32_t t2;

				for (uint32_t v = 0; v < nv; v++)
					vv[v] = &c->vb[b][vo + v];
				vo += nv;
				t2 = R3D_T();
				if (nv < 3 || !r3d_poly_setup(&p, gp, vv, &c->rs)) {
					R3D_ACC(c->st->t_setup, t2);
					continue;
				}
				R3D_ACC(c->st->t_setup, t2);
				c->st->band_polys++;
				if (p.textured)
					tc_bind(c, &p.tex);
				{
					const int32_t ya = p.ytop > r0 ? p.ytop : r0;
					const int32_t yb = p.ybot < r1 - 1 ? p.ybot : r1 - 1;

					for (int32_t y = ya; y <= yb; y++) {
						r3d_row row;
						int ok;

						t2 = R3D_T();
						ok = r3d_row_at(&p, y, &row);
						R3D_ACC(c->st->t_rows, t2);
						if (!ok)
							continue;
						c->st->rows++;
						t2 = R3D_T();
						draw_poly_row(c, &p, &row, col[y - base], c->dep[y - base], c->att[y - base]);
						R3D_ACC(c->st->t_pix, t2);
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
		for (uint32_t b = 0; b < nbands; b++) {
			if (!r3d_job_has_band(j, b))
				continue;
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
	c->cellmode = (j->flags & R3D_F_TEXBLOCK) == 0;
	tc_reset(c);
	memset(c->btick, 0, sizeof c->btick);
	{
		const uint32_t t_ = R3D_T();

		sort_polys(c, hdr, n);
		R3D_ACC(st->t_sort, t_);
	}
	for (uint32_t b = 0; b < nbands; b++) {
		const int32_t y0 = (int32_t)(b * j->band), y1 = y0 + (int32_t)j->band;
		const int edge = (c->rs.d3 & D3_EDGE) && !(c->rs.flags & R3D_F_NOEDGE);
		uint32_t t_, tb_;

		if (!r3d_job_has_band(j, b))
			continue;
		/* el búfer de color `cb` lo usó la salida de hace dos bandas */
		tb_ = R3D_T();
		r3d_wait(1u << (R3D_TAG_OUT + cb));
		draw_band(c, n, y0, y1, cb);
		t_ = R3D_T();
		post_band(c, y0, y1, cb);
		R3D_ACC(st->t_post, t_);
		t_ = R3D_T();
		r3d_put(c->col[cb][edge ? 1 : 0], j->out_ea + (uint64_t)y0 * R3D_W * 4u, j->band * R3D_W * 4u, R3D_TAG_OUT + cb);
		st->dma_bytes += j->band * R3D_W * 4u;
		R3D_ACC(st->t_out, t_);
		if (b < R3D_BT_WORDS)
			R3D_ACC(c->btick[b], tb_);
		st->bands++;
		cb ^= 1u;
	}
	if (j->bt_ea) { /* tiempo por banda para repartir el cuadro siguiente (el PPE lo lee al esperar el trabajo) */
		r3d_put(c->btick, j->bt_ea + 128u * j->unit, sizeof c->btick, R3D_TAG_HDR);
		r3d_wait(1u << R3D_TAG_HDR);
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

R3D_SMALL uint32_t r3d_math_probe(uint32_t seed, uint32_t which)
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
		case 3: { /* división de 32 bits sin signo (la z lineal por cociente y resto, r3d_zlerp) */
			const uint32_t adz = prnd() & 0xFFFFFF, L = 1 + (prnd() & 511);

			v = (uint64_t)(adz / L) << 32 | (adz % L);
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
			static uint32_t col[R3D_W] __attribute__((aligned(16))), dep[R3D_W] __attribute__((aligned(16))),
				att[R3D_W] __attribute__((aligned(16)));
			r3d_rstate rs;
			r3d_poly p;
			r3d_row row;
			r3d_stats st;
			int32_t y;

			if (i >= 256)
				break;
			memset(&rs, 0, sizeof rs);
			memset(&p, 0, sizeof p);
			rs.d3 = D3_BLEND | ((prnd() & 3) == 0 ? D3_ALPHATEST : 0u) | ((prnd() & 3) == 0 ? D3_HIGHLIGHT : 0u);
			rs.alpha_ref = prnd() & 31;
			rs.wbuf = prnd() & 1;
			for (int k = 0; k < 32; k++)
				rs.toon[k] = prnd() & 0x3F3F3Fu;
			p.n = 3;
			p.alpha = (prnd() & 7) == 0 ? 31 : 1 + (prnd() % 31);
			p.id = prnd() & 63;
			p.mode = prnd() & 3;
			p.wire = (prnd() & 15) == 0;
			p.front = prnd() & 1;
			p.wshift = (prnd() & 3) == 0 ? 4 : 0;
			p.attr = (p.alpha << 16) | (p.id << 24) | (p.mode << 4) | ((prnd() & 1) ? PA_TDEPTH : 0u) | ((prnd() & 7) == 0 ? PA_DEQUAL : 0u) |
				 ((prnd() & 1) ? PA_FOG : 0u);
			p.ytop = 0x7FFFFFFF;
			p.ybot = -0x7FFFFFFF;
			for (int k = 0; k < 3; k++) {
				r3d_v *d = &p.v[k];

				d->x = (int32_t)(prnd() % 300) - 20;
				d->y = (int32_t)(prnd() % 200) - 4;
				d->z = prnd() & 0xFFFFFF;
				d->w = 1 + (prnd() & 0x7FFF);
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
				att[x] = prnd() & 0x1FFFF;
			}
			memset(&row, 0, sizeof row);
			memset(&st, 0, sizeof st);
			if (r3d_row_at(&p, y, &row)) {
				r3d_core_row_notex(&rs, &st, &p, &row, col, dep, att);
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
