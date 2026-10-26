/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSXPREP_DECODE_H
#define SALSADS_RSXPREP_DECODE_H
#include "spu/rsxprep/job.h"
/* Ocupa temporalmente el contexto de rasterización: trabajos seriales del mismo
 * trabajador, sin aumentar BSS. 16 líneas de 1 KiB + dos salidas de 4 KiB,
 * descriptor y etiquetas: < 26 KiB. Tags 24/25 salida, 26 lectura, 27 descriptor.
 * El raster reinicializa sus cachés al siguiente trabajo. Enlace exige 8192 B. */
typedef struct rsx_decode_local {
    uint8_t lines[16][1024] __attribute__((aligned(128)));
    uint8_t out[2][4096] __attribute__((aligned(128)));
    uint32_t tags[16];
    rsx_decode_item item;
    rsx_decode_job job;
} rsx_decode_local;
static uint32_t rsx_decode_read(rsx_decode_local *c, uint32_t off)
{
    if (off == R3D_UNMAPPED) return 0;
    unsigned page = off >> 10, line = page & 15;
    c->item.pages[page >> 5] |= 1u << (page & 31);
    if (c->tags[line] != page) {
        spukit_dma_get(c->lines[line], c->job.vram_ea + (off & ~1023u), 1024, 26);
        spukit_dma_wait(1u << 26); c->tags[line] = page;
    }
    return c->lines[line][off & 1023u];
}
static uint32_t rsx_decode_tex(void *v, uint32_t a)
{
    rsx_decode_local *c = v; uint32_t off = c->job.texslot[(a >> 17) & 3];
    return rsx_decode_read(c, off == R3D_UNMAPPED ? off : off + (a & 0x1ffffu));
}
static uint32_t rsx_decode_pal(void *v, uint32_t a)
{
    rsx_decode_local *c = v; unsigned slot = (a >> 14) & 7;
    uint32_t off = slot < 6 ? c->job.palslot[slot] : R3D_UNMAPPED;
    return rsx_decode_read(c, off == R3D_UNMAPPED ? off : off + (a & 0x3fffu));
}
static int rsx_decode_run(rsx_decode_local *c, const rsx_decode_job *job)
{
    c->job = *job;
    memset(c->tags, 255, sizeof c->tags);
    unsigned buffer = 0, pixels = 0;
    for (unsigned i = 0; i < job->count; i++) {
        uint64_t ea = job->items_ea + i * sizeof c->item;
        spukit_dma_get(&c->item, ea, sizeof c->item, 27); spukit_dma_wait(1u << 27);
        rsx_decode_item *d = &c->item;
        if (d->tex.sl < 3 || d->tex.sl > 10 || d->tex.tl < 3 || d->tex.tl > 10) return -1;
        unsigned w = 1u << d->tex.sl, h = 1u << d->tex.tl;
        if (d->first > h || d->rows > h-d->first || d->pitch != ((w*4+63)&~63u) ||
            d->rows*w > RSX_DECODE_PIXELS-pixels) return -1;
        pixels += d->rows*w;
        memset(d->pages, 0, sizeof d->pages);
        for (unsigned y = d->first; y < d->first+d->rows; y++) {
            spukit_dma_wait(1u << (24+buffer));
            for (unsigned x = 0; x < w; x++) {
                uint32_t p = r3d_decode(&d->tex, x, y, rsx_decode_tex, rsx_decode_pal, c);
                uint8_t *b = c->out[buffer]+x*4;
                b[0] = R3D_PX_R(p); b[1] = R3D_PX_G(p); b[2] = R3D_PX_B(p); b[3] = R3D_PX_A(p);
            }
            spukit_dma_put(c->out[buffer], d->out_ea + y*d->pitch, d->pitch, 24+buffer);
            buffer ^= 1;
        }
        spukit_dma_put(d, ea, sizeof *d, 27); spukit_dma_wait(1u << 27);
    }
    spukit_dma_wait((1u << 24) | (1u << 25));
    return 0;
}
#endif
