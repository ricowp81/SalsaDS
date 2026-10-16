/* SalsaDS — Fase 0.5: formato del trabajo entre el PPE y el SPE. */
#ifndef SALSADS_SPE_GPU2D_JOB_H
#define SALSADS_SPE_GPU2D_JOB_H

#include <stdint.h>

#include "gpu2d/gpu2d_compose.h"

#define JOB_COMPOSE 1

/* Parámetros del trabajo (≤ 112 bytes). */
typedef struct job_compose {
	uint64_t frame_ea;   /* gpu2d_frame, alineado a 128 */
	uint32_t engines;    /* bit 0: pantalla A, bit 1: pantalla B */
} job_compose;

/* Resultado (≤ 112 bytes): duración de cada pantalla en ticks del
 * decrementador (79,8 MHz) y sus contadores. */
typedef struct job_result {
	uint32_t ticks[2];
	int32_t rc[2];
	gpu2d_stats stats[2];
	uint32_t ctx_bytes;  /* sizeof(gpu2d_ctx) */
} job_result;

#endif
