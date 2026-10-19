/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.4: núcleo del rasterizador (portable: SPE, PPE y host).
 *
 * r3d_frame dibuja la parte de un cuadro que le toca a un SPE (job->spe_index
 * de job->spe_count) en dos pasadas:
 *
 *   1. setup: trae la lista de triángulos en lotes de R3D_CHUNK por DMA (doble
 *      búfer), calcula bordes y planos de los que tocan alguna de sus unidades
 *      y deja cada registro (struct r3d_rec) en job->scratch_ea; guarda en el
 *      local store la caja de cada triángulo en unidades.
 *   2. dibujo: por cada unidad suya (banda o tile de R3D_UNIT_PIXELS píxeles),
 *      en orden: la limpia, arma la lista de triángulos que la tocan (en orden
 *      de dibujo), los trae con listas de DMA en lotes (doble búfer), los
 *      dibuja con z-buffer de 24 bits y saca el color al cuadro (doble búfer).
 *
 * Devuelve 0 o un código de error (R3D_E*). Los búferes del local store son
 * estáticos: en el host, una sola llamada a la vez.
 */
#ifndef SALSADS_R3D_CORE_H
#define SALSADS_R3D_CORE_H

#include "r3d.h"

enum {
	R3D_EARG = 1,      /* escala, unidad o reparto inválidos */
	R3D_EALIGN = 2,    /* dirección o tamaño mal alineado */
	R3D_ETRIS = 3,     /* demasiados triángulos */
};

int r3d_frame(const struct r3d_job *job, struct r3d_stats *st);

/* Expansión de 5 a 6 bits de la DS (0 → 0, x → 2x + 1). */
static inline uint32_t r3d_x6(uint32_t c5) { return c5 ? c5 * 2u + 1u : 0u; }

/* Texel ya decodificado: (r6+1) | (g6+1) << 8 | (b6+1) << 16 | (a5+1) << 24,
 * a partir de un color BGR555 y un alfa de 5 bits. */
static inline uint32_t r3d_texel(uint32_t bgr555, uint32_t a5)
{
	return (r3d_x6(bgr555 & 31u) + 1u) | (r3d_x6((bgr555 >> 5) & 31u) + 1u) << 8 |
	       (r3d_x6((bgr555 >> 10) & 31u) + 1u) << 16 | (a5 + 1u) << 24;
}

/* Bytes de VRAM que ocupa una textura (0 si el formato no se soporta). */
static inline uint32_t r3d_tex_bytes(uint32_t param)
{
	uint32_t n = R3D_TEX_SIZE_S(param) * R3D_TEX_SIZE_T(param);
	switch (R3D_TEX_FORMAT(param)) {
	case R3D_FMT_PAL16: return n / 2u;
	case R3D_FMT_PAL256: return n;
	case R3D_FMT_DIRECT: return n * 2u;
	default: return 0;
	}
}

static inline uint32_t r3d_pal_bytes(uint32_t param)
{
	switch (R3D_TEX_FORMAT(param)) {
	case R3D_FMT_PAL16: return 32u;
	case R3D_FMT_PAL256: return 512u;
	default: return 0;
	}
}

#endif
