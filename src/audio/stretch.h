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
/* Segunda ruta: grano 40 ms, salto 30 ms, solape 10 ms. El llamador
 * informa n pares disponibles; in contiene min(n, INPUT). Se retira
 * exactamente el valor devuelto; la cola interna se entrega en saltos de 480. */
#ifndef STRETCH2_HOP
#define STRETCH2_HOP 1440u /* 960: grano 30 ms; 1440: grano 40 ms */
#endif
#define STRETCH2_OVERLAP 480u
#define STRETCH2_GRAIN (STRETCH2_HOP + STRETCH2_OVERLAP)
#define STRETCH2_INPUT 5120u
typedef struct audio_stretch2 {
	float grain[2*STRETCH2_GRAIN], output[2*STRETCH2_HOP];
	float filtered[2*STRETCH2_INPUT], reference[2*STRETCH2_OVERLAP];
	float rate, fraction, arrivals;
	uint32_t previous, consumed, initialized, valid, queued, position, ticks, grain_used, credit;
	uint64_t inserted, active, missing, joins;
} audio_stretch2;
uint32_t audio_stretch2_run(audio_stretch2 *s, const float *in, uint32_t n, float *out);
float audio_soft_limit(float x);
#endif
