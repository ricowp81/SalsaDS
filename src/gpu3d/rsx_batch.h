/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_BATCH_H
#define SALSADS_RSX_BATCH_H
#include <stdint.h>
#include "gpu3d/gx_exec.h"
#include "gpu3d/r3d_px.h"

/* Clave de lote del raster RSX: todo lo que un dibujo de color o de atributos
 * fija por polígono (programa, constantes k, textura y pruebas de stencil).
 * Dos comandos contiguos con la misma clave y vértices consecutivos se mandan
 * en un solo dibujo sin cambiar el orden de las primitivas. Portable: el host
 * cuenta dibujos con la misma regla que el PPE. */
typedef struct rsx_batch_key {
    uint32_t texparam, pltt;   /* TEXIMAGE_PARAM (0 sin textura) y PLTT_BASE */
    uint16_t flatz;            /* solo atributos por constantes: Z>>9 del plano */
    uint8_t trans, equal, mode, id, tdepth, front, textured, alpha, wshift;
    uint8_t flat, fog;         /* solo atributos por constantes */
} rsx_batch_key;

enum {
    RSX_BATCH_EXTENDED = 1,   /* 3D ampliado: la cara forma parte del lote */
    RSX_BATCH_ATTR_CONST = 2  /* atributos con Z plana y bandera por constantes */
};

static inline void rsx_batch_key_init(rsx_batch_key *k, const gx_poly *gp, const r3d_poly *p,
                                      const gx_frame *f, const r3d_rstate *rs)
{
    uint32_t fmt = (gp->texparam >> 26) & 7u;
    k->textured = (uint8_t)p->textured;
    k->texparam = p->textured ? gp->texparam & 0x3fffffffu : 0u;
    k->pltt = p->textured && fmt != 7u ? gp->pltt & 0x1fffu : 0u;
    k->trans = p->alpha < 31 || (p->mode != 1 && p->textured && (fmt == 1u || fmt == 6u));
    k->equal = !!(p->attr & PA_DEQUAL);
    k->mode = (uint8_t)p->mode;
    k->id = (uint8_t)p->id;
    k->tdepth = !!(p->attr & PA_TDEPTH);
    k->front = !!(gp->flags & GX_POLY_FRONT);
    k->alpha = (uint8_t)p->alpha;
    k->wshift = (uint8_t)p->wshift;
    k->fog = !!(p->attr & PA_FOG);
    k->flat = 1; k->flatz = 0;
    if (!rs->wbuf && gp->nverts >= 1 && gp->vidx[0] < f->nverts) {
        uint32_t z0 = f->vtx[gp->vidx[0]].z;
        for (unsigned i = 1; i < gp->nverts && i < GX_POLY_MAXV; i++)
            if (gp->vidx[i] >= f->nverts || f->vtx[gp->vidx[i]].z != z0) k->flat = 0;
        k->flatz = (uint16_t)(z0 >> 9);
    } else k->flat = 0;
}

/* Lote posible entre dos comandos contiguos (los vértices los comprueba el
 * llamador). Solo opacos normales: translúcidos, igualdad y sombras conservan
 * sus pasadas por polígono. En 3D ampliado la cara va en el stencil y las
 * dos pasadas frontales se emiten por lote (ver rsx_color_passes). */
static inline int rsx_batch_equal(const rsx_batch_key *a, const rsx_batch_key *b, unsigned flags)
{
    if (a->trans || b->trans || a->equal || b->equal || a->mode == 3 || b->mode == 3) return 0;
    if (a->mode != b->mode || a->id != b->id || a->tdepth != b->tdepth) return 0;
    if (a->textured != b->textured || a->alpha != b->alpha || a->wshift != b->wshift) return 0;
    if (a->texparam != b->texparam || a->pltt != b->pltt) return 0;
    if ((flags & RSX_BATCH_EXTENDED) && a->front != b->front) return 0;
    if ((flags & RSX_BATCH_ATTR_CONST) &&
        (a->flat != b->flat || a->fog != b->fog || (a->flat && a->flatz != b->flatz))) return 0;
    return 1;
}
#endif
