/* SalsaDS — Fase 2.1: formato del trabajo entre el PPE y el SPE del compositor 2D (src/gpu2d). */
#ifndef SALSADS_SPE_GPU2D_JOB_H
#define SALSADS_SPE_GPU2D_JOB_H

#include <stdint.h>

#include "gpu2d/gpu2d_compose.h"

#define JOB_COMPOSE 1

/* Parámetros del trabajo (≤ 112 bytes). */
typedef struct job_compose {
	uint64_t frame_ea;   /* gpu2d_frame, alineado a 128 */
	uint32_t engine;     /* 0: pantalla A, 1: pantalla B */
} job_compose;

/* Resultado (≤ 112 bytes): duración de la pantalla en ticks del decrementador (79,8 MHz) y sus contadores. */
typedef struct job_result {
	uint32_t ticks;
	int32_t rc;
	gpu2d_stats stats;
	uint32_t ctx_bytes;  /* sizeof(gpu2d_ctx) */
} job_result;

#endif
