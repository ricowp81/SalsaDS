/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX3D_H
#define SALSADS_RSX3D_H
#include "gpu3d/r3d.h"
typedef struct rsx3d_stats {
    uint32_t polys, draws, textures, uploaded, cache_hits;
    uint64_t raster_ns, export_ns;
    uint64_t prepare_us, copy_us, total_us;
} rsx3d_stats;
int rsx3d_init(void);
int rsx3d_selftest(void);
/* 1: exportado a out_ea; 0: cuadro no soportado, salida intacta; -1: error. */
int rsx3d_render(const r3d_job *job, rsx3d_stats *stats);
const char *rsx3d_reason(void);
void rsx3d_shutdown(void);
#endif
