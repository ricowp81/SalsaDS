/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_REPLAY_H
#define SALSADS_RSX_REPLAY_H
#include <stdio.h>
#include <string.h>
#include "gpu3d/r3d.h"

/* Cuadro de diagnóstico sin punteros ni dependencia de ABI. Cada escalar ocupa
 * cuatro bytes LE; VRAM conserva sus bytes originales. No publicar volcados. */
static inline int r3d_replay_word(FILE *io, uint32_t *v, int read)
{
    uint8_t b[4];
    if (read) {
        if (fread(b, 1, 4, io) != 4) return 0;
        *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    } else {
        for (unsigned i = 0; i < 4; i++) b[i] = (uint8_t)(*v >> (i * 8));
        if (fwrite(b, 1, 4, io) != 4) return 0;
    }
    return 1;
}
static inline int r3d_replay_io(FILE *io, gx_frame *f, r3d_job *j, uint8_t *vram, int read)
{
    uint32_t magic = 0x31585352u;
    if (!r3d_replay_word(io, &magic, read) || magic != 0x31585352u) return 0;
    if (read) { memset(f, 0, sizeof *f); memset(j, 0, sizeof *j); }
#define RW(x) do { uint32_t value = (uint32_t)(x); if (!r3d_replay_word(io, &value, read)) return 0; if (read) (x) = value; } while (0)
    RW(f->npolys); RW(f->nverts); RW(f->swap_params); RW(f->seq); RW(j->flags);
    if (f->npolys > GX_MAX_POLYS || f->nverts > GX_MAX_VERTS) return 0;
    for (unsigned i = 0; i < 4; i++) RW(j->texslot[i]);
    for (unsigned i = 0; i < 6; i++) RW(j->palslot[i]);
    gx_regs *r = &f->regs;
    RW(r->disp3dcnt); RW(r->clear_depth); RW(r->clrimage_offset); RW(r->fog_offset);
    RW(r->clear_color); RW(r->fog_color); RW(r->alpha_test_ref);
    for (unsigned i = 0; i < 8; i++) RW(r->edge_color[i]);
    for (unsigned i = 0; i < 32; i++) { RW(r->fog_table[i]); RW(r->toon_table[i]); }
    for (unsigned i = 0; i < f->nverts; i++) {
        gx_vertex *v = &f->vtx[i];
        RW(v->sx); RW(v->sy); RW(v->z); RW(v->w); RW(v->s); RW(v->t); RW(v->r); RW(v->g); RW(v->b);
    }
    for (unsigned i = 0; i < f->npolys; i++) {
        gx_poly *p = &f->poly[i];
        RW(p->attr); RW(p->texparam); RW(p->pltt); RW(p->nverts); RW(p->flags); RW(p->wshift);
        RW(p->ymin); RW(p->ymax); RW(p->xmin); RW(p->xmax);
        if (p->nverts < 3 || p->nverts > GX_POLY_MAXV || p->wshift > 28) return 0;
        for (unsigned k = 0; k < p->nverts; k++) { RW(p->vidx[k]); if (p->vidx[k] >= f->nverts) return 0; }
    }
#undef RW
    for (unsigned i = 0; i < 10; i++) {
        uint32_t slot = i < 4 ? j->texslot[i] : j->palslot[i - 4];
        uint32_t size = i < 4 ? 0x20000u : 0x4000u;
        if (slot != R3D_UNMAPPED && slot > R3D_VRAM_ZERO_OFF - size) return 0;
    }
    return (read ? fread(vram, 1, R3D_VRAM_ZERO_OFF, io) : fwrite(vram, 1, R3D_VRAM_ZERO_OFF, io)) == R3D_VRAM_ZERO_OFF;
}
#endif
