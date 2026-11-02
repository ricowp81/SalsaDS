/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_CAPACITY_H
#define SALSADS_RSX_CAPACITY_H
#include "gpu3d/rsx_passes.h"

/* Un TID deja de ser observable después de su última aparición. Reutilizar
 * su código exige borrarlo de TODO el stencil antes del primer polígono del
 * ID siguiente: EQUAL con trans_mask, INVERT con write=reset[index]. Como
 * el código comparado es el mismo, se borran sus bits sin tocar OID, cara,
 * máscara de sombra ni ocupado. La puerta temporal no se consulta.
 *
 * order es el orden efectivo del raster (NULL: orden del cuadro); reset se
 * indexa por polígono original. El borrado se ejecuta aunque ese polígono
 * no llegue a dibujarse: un uso posterior del nuevo ID aún lo necesita.
 * Un ID conserva el mismo código durante todo su intervalo first..last.
 * No usar con niebla por polígono si se recupera el bit de ocupado. */
static inline const char *rsx_capacity_schedule(rsx_shadow_layout *l, const gx_frame *f,
                                               const r3d_rstate *rs, int shadows,
                                               const uint16_t *order, uint8_t reset[GX_MAX_POLYS])
{
    const char *why=shadows ? "sombras: capacidad de IDs simultáneos excedida" :
                            "3D ampliado: capacidad de IDs simultáneos excedida";
    uint8_t writers[64]={0}, readers[64]={0};
    uint16_t first[64], last[64], end[64];
    unsigned no=1, nt=1, bits=0, available=0, used=0;
    const unsigned ids=15u|(shadows ? 0u : 32u)|(R3D_PX_A(rs->clear_px) ? 16u : 0u);
    memset(reset,0,GX_MAX_POLYS);
    memset(first,255,sizeof first); memset(last,0,sizeof last); memset(end,0,sizeof end);
    writers[AT_OID(rs->clear_attr)]=1;
    for (unsigned i=0;i<f->npolys;i++) {
        unsigned index=order ? order[i] : i;
        if (index>=f->npolys) return "orden de polígonos inválido";
        const gx_poly *p=&f->poly[index]; unsigned id=PA_ID(p->attr), mode=PA_MODE(p->attr);
        if (mode==3 && !id) continue;
        if (shadows && mode==3) readers[id]=1;
        if (mode!=3 && PA_ALPHA(p->attr)==31) writers[id]=1;
        if (rsx_poly_translucent(p->attr,p->texparam,rs)) {
            if (first[id]==UINT16_MAX) { first[id]=(uint16_t)i; nt++; }
            last[id]=(uint16_t)i;
        }
    }
    for (unsigned id=0;id<64;id++) no+=readers[id] && writers[id];
    while ((1u<<bits)<no) bits++;
    for (unsigned bit=1;bit<64;bit<<=1) available+=!!(ids&bit);
    l->opaques=no; l->translucents=nt;
    if (bits>=available) return why;
    l->opaque_mask=rsx_id_scatter((1u<<bits)-1,ids);
    l->trans_mask=ids&~l->opaque_mask;
    const unsigned slots=(1u<<(available-bits))-1;
    memset(l->oid,0,sizeof l->oid); memset(l->tid,255,sizeof l->tid);
    memset(l->oid_test,255,sizeof l->oid_test);
    no=1;
    for (unsigned id=0;id<64;id++) if (readers[id] && writers[id])
        l->oid_test[id]=l->oid[id]=rsx_id_scatter(no++,l->opaque_mask);
    for (unsigned i=0;i<f->npolys;i++) {
        unsigned index=order ? order[i] : i, id=PA_ID(f->poly[index].attr);
        if (first[id]!=i) continue;
        /* Un código que aún no se usó evita un barrido completo. Una vez
         * agotados, cualquier intervalo ya terminado puede cederlo. */
        unsigned slot=used;
        if (used==slots) {
            slot=0;
            while (slot<used && end[slot]>=i) slot++;
        }
        if (slot==slots) return why;
        const unsigned code=rsx_id_scatter(slot+1,l->trans_mask);
        if (slot<used) reset[index]=(uint8_t)code;
        else used++;
        end[slot]=last[id];
        l->tid[id]=(uint8_t)code;
    }
    l->ids_mask=ids; l->extended=1; l->single=0;
    return NULL;
}
#endif
