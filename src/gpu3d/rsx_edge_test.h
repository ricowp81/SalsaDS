/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_EDGE_TEST_H
#define SALSADS_RSX_EDGE_TEST_H
#include <string.h>
#include "gpu3d/r3d_px.h"

/* Escenas comunes al banco del host y a la autoprueba de la consola.
 * Los tres bits bajos identifican el grupo de respaldo: W, translúcidos,
 * niebla. Los demás cruzan AA, alfa de fondo, caras y límite de pantalla.
 * 128..143 añaden texeles transparentes interiores: no son aristas.
 * 144..159 añaden máscara y sombra, también con alfa 31 y TDEPTH.
 * 160..167 cruzan aristas inclinadas con Z y W variables. */
#define RSX_EDGE_TESTS 168u
static inline unsigned rsx_edge_test_group(unsigned test) { return (test | (test>=144 && test<160 ? 2u : 0u)) & 7u; }

static inline void rsx_edge_test_quad(gx_frame *f, unsigned n, unsigned id,
                                    unsigned alpha, unsigned depth, int x0, int y0,
                                    int x1, int y1, unsigned rgb, unsigned back)
{
    gx_poly *p=&f->poly[n];
    p->nverts=4; p->flags=back ? 0 : GX_POLY_FRONT;
    if (alpha<31) p->flags|=GX_POLY_TRANSLUCENT;
    p->attr=(id<<24)|(alpha<<16)|(3u<<6)|PA_FOG;
    p->xmin=x0; p->xmax=x1; p->ymin=y0; p->ymax=y1; p->wshift=8;
    for (unsigned k=0;k<4;k++) {
        unsigned ix=4*n+k; p->vidx[k]=ix;
        gx_vertex *v=&f->vtx[ix];
        v->sx=(k==1 || k==2) ? x1 : x0; v->sy=k>=2 ? y1 : y0;
        v->z=depth; v->w=depth;
        v->r=R3D_PX_R(rgb); v->g=R3D_PX_G(rgb); v->b=R3D_PX_B(rgb);
        v->s=(k==1 || k==2) ? 8*16 : 0; v->t=k>=2 ? 8*16 : 0;
    }
}

static inline void rsx_edge_test_vram(uint8_t *vram)
{
    for (unsigned y=0;y<8;y++) for (unsigned x=0;x<8;x++) {
        unsigned off=2*(8*y+x), c=(x>=3 && x<=4 && y>=3 && y<=4) ? 0 : 0xffff;
        vram[off]=c; vram[off+1]=c>>8;
    }
}

static inline void rsx_edge_test_scene(gx_frame *f, unsigned test)
{
    static const unsigned colors[8]={0x001f,0x03e0,0x7c00,0x03ff,0x7fe0,0x7c1f,0x4210,0x0000};
    const unsigned w=test&1, trans=test&2, fog=test&4, aa=test&8;
    const unsigned clear=test&16, back=test&32, screen=test&64;
    const unsigned holes=test>=128 && test<160, shadows=test>=144 && test<160;
    memset(f,0,sizeof *f);
    f->swap_params=w ? 3 : 1;
    f->regs.disp3dcnt=D3_EDGE|D3_BLEND|(aa ? D3_AA : 0)|(fog ? D3_FOG : 0)|(holes ? D3_TEXTURE : 0);
    f->regs.clear_depth=0x7fff;
    f->regs.clear_color=(63u<<24)|(clear ? (31u<<16)|0x4210u : 0)|0x8000u;
    f->regs.fog_color=(31u<<16)|0x03e0u;
    for (unsigned k=0;k<8;k++) f->regs.edge_color[k]=colors[k];
    for (unsigned k=0;k<32;k++) f->regs.fog_table[k]=64;
    unsigned n=0;
    for (unsigned k=0;k<8;k++) {
        unsigned id=k==7 ? 63 : k*8, col=k&3, row=k>>2;
        int x0=col*64+8, x1=col*64+56, y0=row*96+8, y1=row*96+80;
        if (screen) { if (!col) x0=0; if (col==3) x1=256; if (!row) y0=0; else y1=192; }
        rsx_edge_test_quad(f,n++,id,31,0x600000,x0,y0,x1,y1,R3D_PX(63,63,63,0),back && (k&1));
    }
    if (test>=160) for (unsigned k=0;k<8;k++) {
        static const int dx[4]={-3,-9,4,7}, dy[4]={2,-4,3,-2};
        gx_poly *p=&f->poly[k];
        p->xmin=p->ymin=32767; p->xmax=p->ymax=-32767;
        for (unsigned v=0;v<4;v++) {
            gx_vertex *q=&f->vtx[4*k+v];
            q->sx+=dx[v]; q->sy+=dy[v]; q->z+=v*0x10000; q->w+=v*0x20000;
            if (q->sx<p->xmin) p->xmin=q->sx;
            if (q->sx>p->xmax) p->xmax=q->sx;
            if (q->sy<p->ymin) p->ymin=q->sy;
            if (q->sy>p->ymax) p->ymax=q->sy;
        }
    }
    for (unsigned k=0;k<8;k++) {
        unsigned id=k==7 ? 63 : k*8, col=k&3, row=k>>2;
        /* Otro ID de la misma entrada, el mismo ID, vecino delante/detrás
         * y empate exacto. La frontera de oclusión interior no tiene AT_EDGE. */
        if (k&1) id=(id+1)&63;
        unsigned depth=k%3==0 ? 0x500000 : k%3==1 ? 0x700000 : 0x600000;
        rsx_edge_test_quad(f,n++,id,31,depth,col*64+24,row*96+24,col*64+64,row*96+64,
                           R3D_PX(0,0,63,0),0);
        if (k&2) f->poly[n-1].attr&=~PA_FOG;
    }
    if (holes) {
        rsx_edge_test_quad(f,n++,17,31,0x400000,24,72,232,120,R3D_PX(63,63,63,0),0);
        f->poly[n-1].texparam=7u<<26;
    }
    if (trans) {
        /* TDEPTH independiente y TID que coincide con un OID: ambos deben
         * conservar el OID y la candidatura del último opaco. */
        rsx_edge_test_quad(f,n++,8,15,0x300000,0,16,256,56,R3D_PX(63,0,0,0),0);
        rsx_edge_test_quad(f,n++,23,15,0x200000,0,112,256,152,R3D_PX(0,63,0,0),back!=0);
        f->poly[n-1].attr|=PA_TDEPTH;
        if (clear) f->poly[n-2].attr&=~PA_FOG;
    }
    if (shadows) {
        rsx_edge_test_quad(f,n++,0,31,0x700000,20,12,236,180,R3D_PX(0,0,0,0),0);
        f->poly[n-1].attr|=3u<<4;
        rsx_edge_test_quad(f,n++,9,aa ? 31 : 15,0x100000,20,12,236,180,R3D_PX(0,0,0,0),0);
        f->poly[n-1].attr|=(3u<<4)|(aa ? PA_TDEPTH : 0);
    }
    f->npolys=n; f->nverts=4*n;
}
#endif
