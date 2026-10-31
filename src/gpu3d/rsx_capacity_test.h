/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_CAPACITY_TEST_H
#define SALSADS_RSX_CAPACITY_TEST_H
#include <string.h>
#include "gpu3d/r3d_px.h"

/* Rectángulos y profundidades constantes: cada ID deja una región visible.
 * Cada celda enfrenta IDs separados por 16/32, repite el último y vuelve al
 * primero; una colisión del remapeo no puede quedar tapada por otra celda.
 * Las familias 8/9 mantienen todos los IDs vivos a la vez y exigen respaldo.
 * Las 10..13 mantienen un ID vivo hasta el final y retiran los demás entre
 * celdas; el último ID vuelve sobre celdas anteriores después de reciclar.
 * Variantes: W, niebla parcial, toon/resaltado, TDEPTH y destino trasero. */
#define RSX_CAPACITY_TEST_FAMILIES 14u
#define RSX_CAPACITY_TESTS (RSX_CAPACITY_TEST_FAMILIES * 16u)
static inline unsigned rsx_capacity_test_ids(unsigned test)
{
    static const unsigned ids[RSX_CAPACITY_TEST_FAMILIES]={16,31,32,63,8,15,31,7,64,32,64,31,7,63};
    return ids[test>>4];
}
static inline unsigned rsx_capacity_test_shadows(unsigned test)
{
    unsigned family=test>>4;
    return (family>=4 && family<=7) || family==9 || family>=11;
}
static inline unsigned rsx_capacity_test_admitted(unsigned test)
{
    unsigned family=test>>4;
    return family<8 || family>=10;
}
static inline void rsx_capacity_test_quad(gx_frame *f, unsigned id, unsigned alpha,
                                         unsigned mode, unsigned x, unsigned y,
                                         unsigned width, unsigned depth, unsigned rgb,
                                         unsigned flags, unsigned fog, unsigned tdepth)
{
    unsigned n=f->npolys++;
    gx_poly *p=&f->poly[n];
    p->attr=(id<<24)|(alpha<<16)|(mode<<4)|(3u<<6)|(fog ? PA_FOG : 0)|(tdepth ? PA_TDEPTH : 0);
    p->flags=flags; p->nverts=4; p->wshift=8;
    p->xmin=x; p->xmax=x+width; p->ymin=y; p->ymax=y+16;
    for (unsigned k=0;k<4;k++) {
        unsigned ix=f->nverts++; p->vidx[k]=ix;
        gx_vertex *v=&f->vtx[ix];
        v->sx=x+((k==1 || k==2) ? width : 0); v->sy=y+(k>=2 ? 16 : 0);
        v->z=depth; v->w=depth;
        v->r=R3D_PX_R(rgb); v->g=R3D_PX_G(rgb); v->b=R3D_PX_B(rgb);
    }
}
static inline void rsx_capacity_test_scene(gx_frame *f, unsigned test)
{
    const unsigned family=test>>4, count=rsx_capacity_test_ids(test);
    const unsigned temporal=family>=10;
    const unsigned shadows=rsx_capacity_test_shadows(test), back=family==7 || family==12 || !!(test&8), fog=!!(test&2);
    const unsigned toon=!!(test&4), mode=toon ? 2 : 0;
    const unsigned clear=family==0 || family==1 || family==4 || (family>=10 && family<=12) ? 0 : 31;
    memset(f,0,sizeof *f);
    f->swap_params=1|((test&1)<<1); f->regs.clear_depth=0x7fff;
    f->regs.clear_color=(63u<<24)|(clear<<16)|(back ? 0x8000u : 0);
    f->regs.disp3dcnt=D3_BLEND|(fog ? D3_FOG : 0)|(toon && back ? D3_HIGHLIGHT : 0);
    f->regs.fog_color=31u<<16;
    for (unsigned t=0;t<32;t++) {
        f->regs.fog_table[t]=64; /* densidad 1/2: mezcla parcial exacta */
        f->regs.toon_table[t]=(t&1) ? 0x7fffu : 0x03e0u;
    }
    for (unsigned cell=0;cell<count;cell++) {
        const unsigned id=shadows ? cell+1 : cell;
        const unsigned step=count>32 ? 32 : count>16 ? 16 : count/2;
        const unsigned other=(temporal ? 0 : (cell+step)%count)+(shadows ? 1 : 0);
        const unsigned x=16+(cell%8)*28, y=16+(cell/8)*20;
        const unsigned writer=(family==7 || family==12) && id<=3 ? id : 63;
        const unsigned face=back ? 0 : GX_POLY_FRONT;
        /* Media celda sin opaco prueba también el destino transparente. */
        rsx_capacity_test_quad(f,writer,31,mode,x,y,12,0x800000,R3D_PX(63,63,63,0),face,1,0);
        if (shadows) {
            unsigned shadow_alpha=back && !temporal ? 31 : 15;
            /* Alfa 31 ordena máscara y sombra juntas antes de los
             * translúcidos normales, conservando la máscara necesaria. */
            rsx_capacity_test_quad(f,0,shadow_alpha,3,x,y,24,0x900000,0,GX_POLY_FRONT,0,0);
            rsx_capacity_test_quad(f,id,shadow_alpha,3,x,y,24,0x700000,0,GX_POLY_FRONT,1,back);
            /* El mismo ID normal debe ser rechazado después de la sombra. */
            rsx_capacity_test_quad(f,id,15,mode,x,y,24,0x600000,R3D_PX(63,0,0,0),GX_POLY_FRONT,1,back);
            rsx_capacity_test_quad(f,0,15,3,x,y,24,0x900000,0,GX_POLY_FRONT,0,0);
            rsx_capacity_test_quad(f,other,15,3,x,y,24,0x500000,0,GX_POLY_FRONT,!back,back);
            /* Sombra repetida: consumir la máscara sin volver a mezclar. */
            rsx_capacity_test_quad(f,0,15,3,x,y,24,0x900000,0,GX_POLY_FRONT,0,0);
            rsx_capacity_test_quad(f,other,15,3,x,y,24,0x400000,R3D_PX(0,63,0,0),GX_POLY_FRONT,1,back);
            if (temporal) {
                rsx_capacity_test_quad(f,0,15,3,x,y,24,0x900000,0,GX_POLY_FRONT,0,0);
                rsx_capacity_test_quad(f,id,15,3,x,y,24,0x300000,R3D_PX(0,0,63,0),GX_POLY_FRONT,1,back);
            }
        } else {
            rsx_capacity_test_quad(f,id,15,mode,x,y,24,0x800000,R3D_PX(63,0,0,0),GX_POLY_FRONT,1,back);
            f->poly[f->npolys-1].attr|=PA_DEQUAL; /* obliga al reparto ampliado */
            rsx_capacity_test_quad(f,other,15,mode,x,y,24,0x700000,R3D_PX(0,63,0,0),GX_POLY_FRONT,!back,back);
            rsx_capacity_test_quad(f,other,15,mode,x,y,24,0x600000,R3D_PX(0,0,63,0),GX_POLY_FRONT,1,back);
            rsx_capacity_test_quad(f,id,15,mode,x,y,24,0x500000,R3D_PX(63,0,0,0),GX_POLY_FRONT,1,back);
        }
    }
    if (temporal) {
        /* El código del último ID ya se usó en celdas anteriores. No puede
         * rechazar allí el color nuevo ni borrar OID, máscara o ocupación.
         * El ID 0/1 sigue vivo para impedir que se recicle prematuramente. */
        if (shadows) rsx_capacity_test_quad(f,0,15,3,16,16,224,0x900000,0,GX_POLY_FRONT,0,0);
        rsx_capacity_test_quad(f,shadows ? count : count-1,15,shadows ? 3 : mode,
                              16,16,224,0x200000,R3D_PX(0,63,0,0),GX_POLY_FRONT,1,back);
        rsx_capacity_test_quad(f,shadows ? 1 : 0,15,mode,16,36,224,0x100000,
                              R3D_PX(63,0,0,0),GX_POLY_FRONT,1,back);
    }
}
#endif
