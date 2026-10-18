/* SalsaDS — Fase 3.5: formato del trabajo entre el PPE y el SPE del motor de geometría (src/gpu3d/gx_exec.c). */
#ifndef SALSADS_SPE_GEOM_JOB_H
#define SALSADS_SPE_GEOM_JOB_H

#include <stdint.h>

#define JOB_GEOM 1
#define JOB_GEOM_INFO 2 /* sin trabajo: devuelve el tamaño del contexto (comprobación al arrancar) */

/* Parámetros (≤ 112 bytes). `log_ea` es la dirección de la primera palabra del registro de órdenes del trabajo (alineada a 4);
 * `vtx_ea` y `poly_ea` las matrices del cuadro de salida (alineadas a 128); `out_ea` el gx_exec_out que el SPE deja al día
 * (alineado a 16). `flags` son los GX_JOB_*. */
typedef struct job_geom {
	uint64_t log_ea;
	uint64_t vtx_ea;
	uint64_t poly_ea;
	uint64_t out_ea;
	uint32_t nwords;
	uint32_t flags;
} job_geom;

/* Resultado (≤ 112 bytes) */
typedef struct job_geom_result {
	uint32_t ctx_bytes;
	uint32_t cmds;       /* órdenes ejecutadas en este trabajo */
	uint32_t puts;       /* subidas DMA de vértices y polígonos */
	int32_t rc;
} job_geom_result;

#endif
