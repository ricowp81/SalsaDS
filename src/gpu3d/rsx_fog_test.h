/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_FOG_TEST_H
#define SALSADS_RSX_FOG_TEST_H
#include <string.h>
#include "gpu3d/r3d_px.h"

/* Misma escena dirigida en host y consola: todos los AND de bandera,
 * escrituras de profundidad independientes, Z/W y solo alfa. */
#define RSX_FOG_TESTS 512u
static inline void rsx_fog_test_scene(gx_frame *f, unsigned test)
{
    static const unsigned rgb[3][3] = {{63,11,27}, {7,57,19}, {31,5,63}};
    static const unsigned alpha[3] = {31,13,19};
    static const unsigned depth[3] = {0x600000,0x400000,0x200000};
    memset(f, 0, sizeof *f);
    f->npolys = 3; f->nverts = 12; f->swap_params = 1 | ((test & 1) << 1);
    f->regs.clear_depth = 0x7fff;
    f->regs.clear_color = (test & 16) ? 0x8000 : 0;
    f->regs.fog_color = (7u << 16) | 0x7c1f;
    f->regs.fog_offset = 1024;
    f->regs.disp3dcnt = D3_FOG | D3_BLEND | ((test & 8) ? D3_FOG_ALPHA : 0);
    for (unsigned i=0;i<32;i++) f->regs.fog_table[i] = i == 31 ? 127 : 4*i;
    for (unsigned n=0;n<3;n++) {
        gx_poly *p = &f->poly[n];
        p->nverts=4; p->flags=GX_POLY_FRONT;
        p->attr = ((n+1)<<24) | (alpha[n]<<16) | (3u<<6) | ((test & (16u<<n)) ? PA_FOG : 0);
        if ((test & 256) && n==2) p->attr = (p->attr & ~(63u<<24)) | (2u<<24);
        if (n && (test & (1u<<n))) p->attr |= PA_TDEPTH;
        p->xmin=32; p->xmax=224; p->ymin=32; p->ymax=160;
        p->wshift = 8;
        for (unsigned k=0;k<4;k++) {
            unsigned ix=n*4+k; p->vidx[k]=ix;
            gx_vertex *v=&f->vtx[ix];
            v->sx=(k==1 || k==2) ? 224 : 32; v->sy=k>=2 ? 160 : 32;
            if ((test & 128) && !n) v->sy+=192; /* translúcidos sobre borrado transparente */
            v->z=depth[n]; v->w=depth[n];
            v->r=rgb[n][0]; v->g=rgb[n][1]; v->b=rgb[n][2];
        }
    }
}
#endif
