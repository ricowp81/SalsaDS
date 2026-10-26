/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSXPREP_JOB_H
#define SALSADS_RSXPREP_JOB_H
#include "gpu3d/r3d_px.h"
#define JOB_RSX_RGBA8 4
#define RSX_DECODE_PAGES 21u
#define RSX_DECODE_PIXELS 65536u
/* Un lote solo lee la instantánea propia del RSX. No retiene VRAM del núcleo. */
typedef struct rsx_decode_item {
    r3d_tex tex;
    uint64_t out_ea;
    uint32_t pitch, first, rows;
    uint32_t pages[RSX_DECODE_PAGES];
} __attribute__((aligned(128))) rsx_decode_item;
typedef struct rsx_decode_job {
    uint64_t items_ea, vram_ea;
    uint32_t count, texslot[4], palslot[6];
} rsx_decode_job;
_Static_assert(sizeof(rsx_decode_job) <= 112, "parámetros de spukit");
_Static_assert(sizeof(rsx_decode_item) == 256, "descriptor DMA y presupuesto XDR");
#endif
