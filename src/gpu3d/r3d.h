/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — motor de render 3D de la DS (Fase 3.2): rasteriza un `gx_frame` (la lista de polígonos en pantalla que
 * deja el motor de geometría de la 3.1) en una imagen de 256×192 que el motor 2D A usa como su capa BG0.
 *
 * Código propio, escrito desde GBATEK y la comparación de comportamiento con melonDS ejecutándolo (docs/fase3.md,
 * sección 3.2). Ni se leyó ni se copió código de melonDS, NooDS ni DeSmuME.
 *
 * Tres piezas, el mismo resultado bit a bit:
 *   r3d_px.h    las reglas de la DS (bordes, interpolación con corrección por w, texturas, modos de polígono, prueba de
 *               profundidad, translúcidos, sombras, niebla, bordes): funciones `static inline` en enteros, compartidas
 *   r3d_ref.c   la referencia: el cuadro entero en memoria, polígono por polígono, cada texel leído de la VRAM
 *   r3d_core.c  el rasterizador por bandas de líneas que corre en los SPE (y, con la E/S de memcpy, en el host y el PPE):
 *               lista ordenada en el local store, polígonos y vértices por DMA, caché de texturas por bloques
 *               decodificados con LRU, una banda en el local store mientras se dibuja
 *
 * Todo es aritmética entera (con 64 bits donde hace falta): el SPE, el PPE y el host dan los mismos píxeles.
 *
 * Salida: 256 × 192 palabras de 32 bits en el orden de bytes de quien las lee (R3D_PX_*): color de 6 bits por canal y
 * alfa de 5 bits; alfa 0 = transparente (la capa 3D deja ver lo de abajo).
 */
#ifndef SALSADS_GPU3D_R3D_H
#define SALSADS_GPU3D_R3D_H

#include <stdint.h>

#include "gpu3d/gx.h"

#define R3D_W 256
#define R3D_H 192

/* Píxel de salida */
#define R3D_PX(r, g, b, a) ((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16) | ((uint32_t)(a) << 24))
#define R3D_PX_R(p) ((p) & 63u)
#define R3D_PX_G(p) (((p) >> 8) & 63u)
#define R3D_PX_B(p) (((p) >> 16) & 63u)
#define R3D_PX_A(p) (((p) >> 24) & 31u)

/* Memoria de texturas: 4 ranuras de 128 KB (bancos A-D en modo textura) y 6 de paletas de 16 KB (E: 0-3, F/G: 0, 1,
 * 4, 5). Cada ranura es un desplazamiento dentro de la instantánea de la VRAM del núcleo (los 9 bancos seguidos, 656
 * KB, más 128 bytes en cero) o R3D_UNMAPPED (se lee cero, como la referencia 2D). */
#define R3D_UNMAPPED 0xFFFFFFFFu
#define R3D_VRAM_ZERO_OFF 0xA4000u /* G2D_VRAM_SIZE: los ceros que siguen a la instantánea */
#define R3D_TEX_SLOTS 4
#define R3D_PAL_SLOTS 6

/* Banderas del trabajo */
#define R3D_F_NOEDGE 1u   /* no hacer edge marking aunque DISP3DCNT lo pida (diagnóstico) */
#define R3D_F_NOFOG 2u    /* ídem, niebla */
#define R3D_F_BLANK 4u    /* no hay cuadro de geometría: sale transparente */
#define R3D_F_TEXBLOCK 8u /* texturas: bloques de 256 texeles decodificados enteros (la caché de la 3.2 a la 3.8) en lugar de celdas de 4×4 */

/* Un trabajo: un cuadro, repartido en bandas de `band` líneas entre `nunits` unidades (SPE); esta es la `unit`. Las
 * bandas se reparten alternas: la unidad u dibuja las bandas u, u + nunits, u + 2·nunits... (≤ 112 bytes: va dentro
 * de un trabajo de spukit) */
typedef struct r3d_job {
	uint64_t frame_ea; /* gx_frame (alineado a 128) */
	uint64_t vram_ea;  /* instantánea de la VRAM (alineada a 128) */
	uint64_t out_ea;   /* R3D_W × R3D_H palabras de 32 bits (alineado a 128) */
	uint32_t texslot[R3D_TEX_SLOTS];
	uint32_t palslot[R3D_PAL_SLOTS];
	uint32_t nunits, unit;
	uint32_t band;     /* líneas por banda: 8, 16 (normal), 32 o 64; divisor de 192 */
	uint32_t flags;    /* R3D_F_* */
	/* Reparto por costo (r3d_balance): si nunits == 2 y alguna máscara es distinta de cero, la unidad `u` dibuja las bandas
	 * cuyo bit está en bandsel[u] (bit b = banda b). Todo en cero: el reparto alterno de arriba. */
	uint32_t bandsel[2];
	/* Tiempo por banda (3.9): si no es cero, cada unidad escribe ahí, en bt_ea + 128·unidad, 32 palabras (alineado a 128): los
	 * ticks que tardó en dibujar cada banda suya (cero en las que no le tocaron). El PPE los usa para repartir el cuadro siguiente. */
	uint64_t bt_ea;
} __attribute__((aligned(16))) r3d_job;
#define R3D_BT_WORDS 32

/* ¿La unidad del trabajo dibuja la banda `b`? */
static inline int r3d_job_has_band(const r3d_job *j, uint32_t b)
{
	if (j->nunits == 2 && (j->bandsel[0] | j->bandsel[1]))
		return (int)((j->bandsel[j->unit] >> b) & 1u);
	return b >= j->unit && ((b - j->unit) % j->nunits) == 0;
}

/* Contadores de un trabajo (una unidad) */
typedef struct r3d_stats {
	uint32_t polys;       /* polígonos del cuadro (los que llegan del motor de geometría) */
	uint32_t band_polys;  /* pares banda·polígono dibujados */
	uint32_t rows;        /* líneas de polígono recorridas */
	uint32_t pixels;      /* píxeles que pasaron la cobertura */
	uint32_t written;     /* píxeles escritos (color o sombra) */
	uint32_t tex_px;      /* texeles pedidos */
	uint32_t tex_hits;    /* de ellos, en el bloque (celda) del texel anterior o en la caché */
	uint32_t tex_misses;  /* bloques (celdas) decodificados (fallos de la caché) */
	uint32_t tex_evict;   /* bloques (celdas) desalojados */
	uint32_t pal_miss;    /* de line_miss, las de la memoria de paletas */
	uint32_t line_miss;   /* líneas de VRAM traídas por DMA (128 bytes en el modo de bloques, 1 KB en el de celdas) */
	uint32_t bands;       /* bandas dibujadas */
	uint32_t dma_bytes;   /* polígonos, vértices y salida */
	uint32_t pad;
	/* tiempos (ticks del decrementador del SPE, 79,8 MHz; en el host, ceros) */
	uint32_t t_sort, t_list, t_fetch, t_raster, t_tex, t_post, t_out, t_total;
	/* desglose de t_raster (Fase 3.4): preparar polígonos, calcular aristas de cada línea, dibujar píxeles (incluye texturas) */
	uint32_t t_setup, t_rows, t_pix;
} r3d_stats;

/* ---- Caché de texturas automática (3.9) ------------------------------------------------------------------- */
/* Se empieza con los bloques de 256 texeles (la caché de la 3.2 a la 3.8, que va bien con polígonos grandes que usan la mayor
 * parte de cada textura: Super Mario 64 DS, New Super Mario Bros.) y se pasa a las celdas de 4×4 cuando los bloques se desbordan:
 * la relación entre texeles pedidos y texeles decodificados (bloques × 256) cae por debajo de 0,3 (Mario Kart DS: 0,08; NSMB en
 * un nivel 1,0; SM64DS 3,6). De las celdas se vuelve a los bloques con más de 12 texeles pedidos por celda decodificada (SM64DS
 * 58; NSMB en un nivel 20; MKDS 6 a 7). Con estadísticas suavizadas (3/4 + 1) y al menos 8 cuadros entre cambios. No cambia la
 * imagen, solo el tiempo. `st` son los contadores de un cuadro de todas las unidades. */
typedef struct r3d_texpol {
	uint32_t cells;     /* 0: bloques; 1: celdas */
	uint32_t req, dec;  /* texeles pedidos y bloques (celdas) decodificados, suavizados */
	uint32_t hold;      /* cuadros desde el último cambio (satura en 8) */
} r3d_texpol;

static inline void r3d_texpol_feed(r3d_texpol *p, const r3d_stats *st)
{
	if (st->tex_px == 0)
		return;
	p->req = p->req - (p->req >> 2) + st->tex_px;
	p->dec = p->dec - (p->dec >> 2) + st->tex_misses;
	if (p->hold < 8u) {
		p->hold++;
		return;
	}
	if (!p->cells && (uint64_t)p->req * 10u < (uint64_t)p->dec * 256u * 3u && p->dec > 64u * 4u)
		p->cells = 1;
	else if (p->cells && p->req > p->dec * 12u)
		p->cells = 0;
	else
		return;
	p->req = p->dec = 0;
	p->hold = 0;
}

/* ---- Referencia (r3d_ref.c) ------------------------------------------------------------------------- */
/* Rasteriza `f` con la VRAM de la instantánea `vram` (R3D_VRAM_ZERO_OFF bytes + 128 en cero) en `out`. `f` = NULL:
 * transparente. */
void r3d_render_ref(const gx_frame *f, const uint8_t *vram, const uint32_t texslot[R3D_TEX_SLOTS],
		    const uint32_t palslot[R3D_PAL_SLOTS], uint32_t flags, uint32_t *out);

/* ---- Rasterizador por bandas (r3d_core.c) -------------------------------------------------------------- */
/* El contexto (estado y búferes de la unidad) es grande: en el SPE vive en el local store (r3d_core_ctx_size). */
typedef struct r3d_ctx r3d_ctx;
uint32_t r3d_core_ctx_size(void);
/* Dibuja las bandas de la unidad del trabajo. 0 = bien; < 0, trabajo inválido. */
int r3d_core_render(r3d_ctx *c, const r3d_job *j, r3d_stats *st);

/* Sonda de aritmética para la autoprueba (el mismo resultado en el SPE, el PPE y el host): `which` 0..8. */
uint32_t r3d_math_probe(uint32_t seed, uint32_t which);
#define R3D_NPROBES 9

/* Ranuras de textura y paleta a partir de VRAMCNT (bancos A-G, 7 bytes) y del desplazamiento de cada banco en la
 * instantánea (bank_off[0..6]). */
void r3d_slots_from_vramcnt(const uint8_t vramcnt[7], const uint32_t bank_off[7], uint32_t texslot[R3D_TEX_SLOTS],
			    uint32_t palslot[R3D_PAL_SLOTS]);

/* FNV-1a de 64 bits sobre los valores (no los bytes) de una imagen: igual en LE y en BE */
uint64_t r3d_hash_image(const uint32_t *img);

/* ---- Reparto de bandas por costo -------------------------------------------------------------------------- */
/* Reparte las bandas entre dos unidades de mayor a menor costo, cada una a la unidad menos cargada (LPT). Resultado en
 * `job->bandsel`. El costo de cada banda sale de la historia (`hist`: lo que tardó cada banda en un cuadro anterior, medido por
 * los SPE) si hay y el cuadro se parece al que la produjo (`hist_est` es la estimación total de aquel cuadro; se usa la historia con
 * entre 0,6 y 1,6 veces la estimación total de este); si no, de una estimación con las extensiones en pantalla de los polígonos
 * (por banda y polígono: la preparación; por línea: las aristas; por píxel de la caja, la mitad cubierto: el color, más si lleva
 * textura). Con la historia el reparto queda a 1-2 % del ideal en Mario Kart DS y la estimación a 10 % (docs/fase3.md, 3.9). Devuelve
 * la estimación total de este cuadro (para guardarla junto con la historia que produzca). El resultado de la imagen no depende del
 * reparto (cada banda es independiente), solo el tiempo. */
static inline uint32_t r3d_balance(const gx_frame *f, uint32_t band, r3d_job *job, const uint32_t *hist, uint32_t hist_est)
{
	uint32_t cost[R3D_H / 8];
	uint32_t order[R3D_H / 8];
	uint32_t load[2] = { 0, 0 }, nb, n, total = 0;

	job->bandsel[0] = job->bandsel[1] = 0;
	if (!f || band == 0 || band > 64 || (R3D_H % band) != 0)
		return 0;
	nb = R3D_H / band;
	for (uint32_t b = 0; b < nb; b++)
		cost[b] = 0;
	n = f->npolys > GX_MAX_POLYS ? GX_MAX_POLYS : f->npolys;
	for (uint32_t i = 0; i < n; i++) {
		const gx_poly *p = &f->poly[i];
		int32_t y0 = p->ymin, y1 = p->ymax, x0 = p->xmin, x1 = p->xmax;
		uint32_t w, ppx;

		if (y1 < 0 || y0 >= (int32_t)R3D_H || x1 < 0 || x0 >= (int32_t)R3D_W)
			continue;
		if (y0 < 0)
			y0 = 0;
		if (y1 > (int32_t)R3D_H - 1)
			y1 = (int32_t)R3D_H - 1;
		if (x0 < 0)
			x0 = 0;
		if (x1 > (int32_t)R3D_W - 1)
			x1 = (int32_t)R3D_W - 1;
		w = (uint32_t)(x1 - x0 + 1);
		ppx = ((p->texparam >> 26) & 7u) ? 5u : 2u;
		for (uint32_t b = (uint32_t)y0 / band; b <= (uint32_t)y1 / band; b++) {
			const int32_t ra = (int32_t)(b * band) > y0 ? (int32_t)(b * band) : y0;
			const int32_t rb = (int32_t)(b * band + band - 1) < y1 ? (int32_t)(b * band + band - 1) : y1;
			const uint32_t rows = (uint32_t)(rb - ra + 1);

			cost[b] += 120u + rows * (14u + (w * ppx) / 2u);
		}
	}
	for (uint32_t b = 0; b < nb; b++)
		total += cost[b];
	if (hist && hist_est && (uint64_t)total * 10u >= (uint64_t)hist_est * 6u && (uint64_t)total * 10u <= (uint64_t)hist_est * 16u) {
		uint64_t hsum = 0;

		for (uint32_t b = 0; b < nb; b++)
			hsum += hist[b];
		if (hsum)
			for (uint32_t b = 0; b < nb; b++)
				cost[b] = hist[b];
	}
	for (uint32_t b = 0; b < nb; b++) {
		uint32_t k = b;

		while (k > 0 && cost[order[k - 1]] < cost[b]) { /* inserción: de mayor a menor, estable */
			order[k] = order[k - 1];
			k--;
		}
		order[k] = b;
	}
	for (uint32_t k = 0; k < nb; k++) {
		const uint32_t b = order[k], u = load[1] < load[0] ? 1u : 0u;

		load[u] += cost[b] + 1u;
		job->bandsel[u] |= 1u << b;
	}
	return total;
}

#endif
