/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_AUDIO_STRETCH_H
#define SALSADS_AUDIO_STRETCH_H
#include <stdint.h>
#define STRETCH_HOP 480u
#define STRETCH_OVERLAP 240u
#define STRETCH_GRAIN (STRETCH_HOP + STRETCH_OVERLAP)
#define STRETCH_HISTORY 1920u
/* Estado exclusivo del consumidor; sin reservas ni reloj en el núcleo. */
typedef struct audio_stretch {
	float history[2 * STRETCH_HISTORY], overlap[2 * STRETCH_OVERLAP];
	uint32_t history_n, blending;
	uint64_t inserted, active, missing;
} audio_stretch;
/* Produce 480 pares; entrada contigua de hasta 720 pares. Devuelve los consumidos.
 * Con >=720 pares y sin cola de solape la copia es exacta. */
uint32_t audio_stretch_run(audio_stretch *s, const float *in, uint32_t n, float *out);
float audio_soft_limit(float x);
#endif
