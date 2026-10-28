/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_PASSES_H
#define SALSADS_RSX_PASSES_H
#include <stdint.h>

/* Plan portable de profundidad/stencil para polígonos de color. Las sombras
 * conservan su secuencia propia. Valores verificados contra rsxkit al compilar. */
enum { RP_OFF=0, RP_NONE=3, RP_BLEND=4 };
enum { RP_TEST=1, RP_WRITE=2, RP_LESS=16, RP_GEQUAL=32, RP_ALWAYS=64 };
enum { RP_COLOR, RP_MASK, RP_RANGE };
typedef struct rsx_pass {
    uint8_t test, ref, compare, write, op, blend, depth, pass, program, zfail;
    int8_t range;
} rsx_pass;
typedef struct rsx_pass_input {
    unsigned id, tid, oid, trans_mask, front, trans, opaque, tdepth, equal;
    unsigned extended, blend, occupied, attributes, prior_back;
} rsx_pass_input;

static inline unsigned rsx_color_passes(const rsx_pass_input *c, rsx_pass out[9])
{
    unsigned n=0;
#define RP_ADD(t,r,cmp,wr,op,b,z,p,prog,zf,rg) \
    (out[n++]=(rsx_pass){t,r,cmp,wr,op,b,z,p,prog,zf,rg})
    const unsigned z=RP_TEST|RP_LESS, zw=z|(c->tdepth ? RP_WRITE : 0);
    if (!c->extended) {
        if (c->opaque) RP_ADD(1,64,255,255,0,RP_OFF,z|RP_WRITE,0,RP_COLOR,0,0);
        if (c->trans) {
            /* Llenar primero los vacíos (bit 7). Al escribir el ID ya quedan
             * excluidos de la mezcla siguiente, incluso sin escribir Z.
             * Atributos hace AND también sobre un destino transparente. */
            if (!c->attributes && c->blend && !c->occupied)
                RP_ADD(2,c->id,128,255,0,RP_OFF,zw,2,RP_COLOR,0,0);
            RP_ADD(2,c->id,255,255,0,c->blend ? RP_BLEND : RP_OFF,zw,
                   c->blend ? 1 : 2,RP_COLOR,0,0);
        }
    } else if (!c->trans && !c->equal) {
        /* Un opaco normal no necesita puerta temporal. El ref frontal tiene
         * bit 6 cero: NOTEQUAL consulta una espalda y REPLACE la borra.
         * La máscara de sombra (bit 5) permanece intacta. */
        unsigned ref=16|c->oid;
        if (c->front && c->prior_back) {
            RP_ADD(3,ref,64,95,0,RP_OFF,z|RP_WRITE,0,RP_COLOR,0,0);
            RP_ADD(2,ref,64,95,0,RP_OFF,RP_TEST|RP_WRITE,0,RP_COLOR,0,0);
        } else RP_ADD(1,ref|(c->front ? 0 : 64),0,95,0,RP_OFF,z|RP_WRITE,0,RP_COLOR,0,0);
    } else {
        unsigned back=c->front ? 0 : 64;
        RP_ADD(1,0,0,128,0,RP_NONE,0,0,RP_MASK,0,0);
        if (c->equal) {
            RP_ADD(1,128,0,128,0,RP_NONE,RP_TEST,0,RP_RANGE,0,-1);
            RP_ADD(1,0,0,128,2,RP_NONE,RP_TEST|RP_GEQUAL,0,RP_RANGE,2,1);
        } else if (c->front && c->prior_back) {
            RP_ADD(3,128,64,128,0,RP_NONE,z,0,RP_MASK,0,0);
            RP_ADD(3,192,64,128,0,RP_NONE,RP_TEST,0,RP_MASK,0,0);
        } else RP_ADD(1,128,0,128,0,RP_NONE,z,0,RP_MASK,0,0);
        if (c->opaque)
            RP_ADD(3,128|16|back|c->oid,128,95,0,RP_OFF,
                   RP_TEST|RP_WRITE|RP_ALWAYS,0,RP_COLOR,0,0);
        if (c->trans) {
            RP_ADD(3,c->tid,c->trans_mask,128,0,RP_NONE,0,0,RP_MASK,0,0);
            unsigned wz=c->trans_mask|(c->tdepth ? 64 : 0);
            unsigned dz=RP_TEST|RP_ALWAYS|(c->tdepth ? RP_WRITE : 0);
            if (c->attributes || c->occupied || !c->blend) {
                /* El rechazo de ID ya apagó la puerta. Escribir ocupado,
                 * TID y (si procede) cara junto con color/atributos. */
                RP_ADD(3,128|16|back|c->tid,128,wz|16,0,
                       c->blend ? RP_BLEND : RP_OFF,dz,c->blend ? 1 : 2,RP_COLOR,0,0);
            } else {
                RP_ADD(3,128|16|back|c->tid,144,wz,0,RP_BLEND,dz,1,RP_COLOR,0,0);
                RP_ADD(3,128|back|c->tid,144,wz,0,RP_OFF,dz,2,RP_COLOR,0,0);
                RP_ADD(3,16|c->tid,c->trans_mask,16,0,RP_NONE,0,2,RP_COLOR,0,0);
            }
        }
    }
#undef RP_ADD
    return n;
}
#endif
