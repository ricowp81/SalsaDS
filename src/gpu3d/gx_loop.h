/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ejecutor de prueba del modo diferido del motor de geometría (Fase 3.5): ejecuta los trabajos en el acto, en el
 * mismo hilo, con ventanas de salida chicas y el registro por tramos, igual que lo hace el SPE de geometría (src/spu/geom).
 * Sirve para probar en el host, en little y big-endian, que el modo diferido da los mismos bits que el directo.
 */
#ifndef SALSADS_GPU3D_GX_LOOP_H
#define SALSADS_GPU3D_GX_LOOP_H

#include "gpu3d/gx.h"

#define GX_LOOP_VCAP 24u
#define GX_LOOP_PCAP 3u
#define GX_LOOP_CHUNK 96u

typedef struct gx_loop {
	gx_exec ex;
	gx_vertex vst[GX_LOOP_VCAP];
	gx_poly pst[GX_LOOP_PCAP];
	uint32_t chunk[GX_LOOP_CHUNK];
	const uint32_t *log;
	gx_frame *f;
	uint32_t seq, jobs;
} gx_loop;

/* Pone `g` en modo diferido con este ejecutor (llama a gx_set_executor). */
void gx_loop_attach(gx_t *g, gx_loop *lp);

#endif
