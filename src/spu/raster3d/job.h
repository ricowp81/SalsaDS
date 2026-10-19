/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — Fase 3.2: formato del trabajo entre el PPE y los SPE del render 3D (src/gpu3d/r3d_core.c). */
#ifndef SALSADS_SPE_RASTER3D_JOB_H
#define SALSADS_SPE_RASTER3D_JOB_H

#include <stdint.h>

#include "gpu3d/r3d.h"

#define JOB_RASTER3D 1
#define JOB_RASTER3D_INFO 2 /* sin trabajo: devuelve el tamaño del contexto (comprobación al arrancar) */
#define JOB_RASTER3D_PROBE 3 /* sondas de aritmética: data = semilla; resultado en probe[] */

/* Parámetros: el r3d_job tal cual (≤ 112 bytes). Resultado: los contadores de la unidad (r3d_stats, 88 bytes) y el
 * código de vuelta. */
typedef struct job_r3d_result {
	union {
		r3d_stats st;
		uint32_t probe[R3D_NPROBES]; /* JOB_RASTER3D_PROBE */
	};
	int32_t rc;
	uint32_t ctx_bytes;
} job_r3d_result;

#endif
