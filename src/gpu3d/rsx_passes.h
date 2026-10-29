/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_PASSES_H
#define SALSADS_RSX_PASSES_H
#include <stdint.h>
#include <string.h>
#include "gpu3d/r3d_px.h"

/* Plan portable de profundidad/stencil para polígonos de color. Las sombras
 * conservan su secuencia propia. Valores verificados contra rsxkit al compilar. */
enum { RP_OFF=0, RP_NONE=3, RP_BLEND=4 };
enum { RP_TEST=1, RP_WRITE=2, RP_LESS=16, RP_GEQUAL=32, RP_ALWAYS=64, RP_EQUAL=128 };
enum { RP_COLOR, RP_MASK, RP_RANGE };
/* zfail/sfail: 0 conservar, 1 reemplazar, 2 poner a cero (al fallar la
 * profundidad o el stencil). La máscara de escritura limita ambos. */
typedef struct rsx_pass {
    uint8_t test, ref, compare, write, op, blend, depth, pass, program, zfail, sfail;
    int8_t range;
} rsx_pass;
typedef struct rsx_pass_input {
    unsigned id, tid, oid, trans_mask, front, trans, opaque, tdepth, equal;
    unsigned extended, blend, occupied, attributes, prior_back;
} rsx_pass_input;

/* Con `compact` la puerta del bit 7 queda decidida por completo en sus
 * propias pasadas (1 si pasa, 0 si falla profundidad o stencil): sobra la
 * pasada de borrado, y el rechazo del mismo ID translúcido entra en la misma
 * prueba cuando el polígono no tiene texeles opacos. Sin `compact`, el plan
 * de la ronda 6 (A/B: 3d-rsx-puerta-ronda6). */
static inline unsigned rsx_color_passes_mode(const rsx_pass_input *c, rsx_pass out[9], int compact)
{
    unsigned n=0;
#define RP_ADD(t,r,cmp,wr,op,b,z,p,prog,zf,sf,rg) \
    (out[n++]=(rsx_pass){t,r,cmp,wr,op,b,z,p,prog,zf,sf,rg})
    const unsigned z=RP_TEST|RP_LESS, zw=z|(c->tdepth ? RP_WRITE : 0);
    if (!c->extended) {
        if (c->opaque) RP_ADD(1,64,255,255,0,RP_OFF,z|RP_WRITE,0,RP_COLOR,0,0,0);
        if (c->trans) {
            /* Llenar primero los vacíos (bit 7). Al escribir el ID ya quedan
             * excluidos de la mezcla siguiente, incluso sin escribir Z.
             * Atributos hace AND también sobre un destino transparente. */
            if (!c->attributes && c->blend && !c->occupied)
                RP_ADD(2,c->id,128,255,0,RP_OFF,zw,2,RP_COLOR,0,0,0);
            RP_ADD(2,c->id,255,255,0,c->blend ? RP_BLEND : RP_OFF,zw,
                   c->blend ? 1 : 2,RP_COLOR,0,0,0);
        }
    } else if (!c->trans && !c->equal) {
        /* Un opaco normal no necesita puerta temporal. El ref frontal tiene
         * bit 6 cero: NOTEQUAL consulta una espalda y REPLACE la borra.
         * La máscara de sombra (bit 5) permanece intacta. Primero las
         * espaldas (LEQUAL) y después los frentes (LESS): así un lote de
         * varios polígonos frontales equivale al orden polígono a polígono
         * (una espalda ganada por uno deja un frente que el siguiente solo
         * supera con LESS, igual que en la secuencia original). */
        unsigned ref=16|c->oid;
        if (c->front && c->prior_back) {
            RP_ADD(2,ref,64,95,0,RP_OFF,RP_TEST|RP_WRITE,0,RP_COLOR,0,0,0);
            RP_ADD(3,ref,64,95,0,RP_OFF,z|RP_WRITE,0,RP_COLOR,0,0,0);
        } else RP_ADD(1,ref|(c->front ? 0 : 64),0,95,0,RP_OFF,z|RP_WRITE,0,RP_COLOR,0,0,0);
    } else {
        unsigned back=c->front ? 0 : 64;
        /* Rechazo del mismo ID dentro de la puerta: solo si ningún texel
         * opaco debe ignorarlo. */
        unsigned fold=compact && c->trans && !c->opaque;
        unsigned gate_test=fold ? 2 : 1, gate_ref=fold ? c->tid : 0;
        unsigned gate_cmp=fold ? c->trans_mask : 0;
        if (!compact) RP_ADD(1,0,0,128,0,RP_NONE,0,0,RP_MASK,0,0,0);
        if (c->equal) {
            if (compact) RP_ADD(gate_test,gate_ref|128,gate_cmp,128,0,RP_NONE,RP_TEST,0,RP_RANGE,2,2,-1);
            else RP_ADD(1,128,0,128,0,RP_NONE,RP_TEST,0,RP_RANGE,0,0,-1);
            RP_ADD(1,0,0,128,2,RP_NONE,RP_TEST|RP_GEQUAL,0,RP_RANGE,2,0,1);
        } else if (c->front && c->prior_back) {
            if (compact) {
                /* α: LEQUAL (e ID distinto); β: apaga la puerta donde el
                 * destino es un frente con la misma profundidad (el frente
                 * solo gana el empate a una espalda). */
                RP_ADD(gate_test,gate_ref|128,gate_cmp,128,0,RP_NONE,RP_TEST,0,RP_MASK,2,2,0);
                RP_ADD(3,0,64,128,0,RP_NONE,RP_TEST|RP_EQUAL,0,RP_MASK,0,0,0);
            } else {
                RP_ADD(3,128,64,128,0,RP_NONE,z,0,RP_MASK,0,0,0);
                RP_ADD(3,192,64,128,0,RP_NONE,RP_TEST,0,RP_MASK,0,0,0);
            }
        } else if (compact) {
            RP_ADD(gate_test,gate_ref|128,gate_cmp,128,0,RP_NONE,z,0,RP_MASK,2,2,0);
        } else RP_ADD(1,128,0,128,0,RP_NONE,z,0,RP_MASK,0,0,0);
        if (c->opaque)
            RP_ADD(3,128|16|back|c->oid,128,95,0,RP_OFF,
                   RP_TEST|RP_WRITE|RP_ALWAYS,0,RP_COLOR,0,0,0);
        if (c->trans) {
            if (!fold) RP_ADD(3,c->tid,c->trans_mask,128,0,RP_NONE,0,0,RP_MASK,0,0,0);
            unsigned wz=c->trans_mask|(c->tdepth ? 64 : 0);
            unsigned dz=RP_TEST|RP_ALWAYS|(c->tdepth ? RP_WRITE : 0);
            if (c->attributes || c->occupied || !c->blend) {
                /* El rechazo de ID ya apagó la puerta. Escribir ocupado,
                 * TID y (si procede) cara junto con color/atributos. */
                RP_ADD(3,128|16|back|c->tid,128,wz|16,0,
                       c->blend ? RP_BLEND : RP_OFF,dz,c->blend ? 1 : 2,RP_COLOR,0,0,0);
            } else {
                RP_ADD(3,128|16|back|c->tid,144,wz,0,RP_BLEND,dz,1,RP_COLOR,0,0,0);
                RP_ADD(3,128|back|c->tid,144,wz,0,RP_OFF,dz,2,RP_COLOR,0,0,0);
                RP_ADD(3,16|c->tid,c->trans_mask,16,0,RP_NONE,0,2,RP_COLOR,0,0,0);
            }
        }
    }
#undef RP_ADD
    return n;
}
static inline unsigned rsx_color_passes(const rsx_pass_input *c, rsx_pass out[9])
{
    return rsx_color_passes_mode(c,out,1);
}

/* ---- Sombras (polígonos de modo 3) --------------------------------------------------------------- */
/* El mismo plan que el PPE manda al RSX, en forma portable para que el banco
 * del host lo ejecute contra las reglas escalares de la DS (r3d_px.h). Tres
 * repartos de stencil:
 *   sin ampliar, una sola sombra  bit 7 ocupado, 5 máscara, 4 último ID de
 *                                 sombra, 1..3 otros TID, 0 OID igual a sombra
 *   sin ampliar, varias sombras   bit 7 ocupado, 6 temporal, 5 máscara,
 *                                 2..4 TID, 0..1 OID
 *   ampliado (`extended`)         bit 7 puerta, 6 cara trasera, 5 máscara,
 *                                 4 ocupado, TID y OID según trans_mask/opaque_mask
 * `consume` (una sola sombra): la máscara se apaga también si falla la
 * profundidad, en la misma pasada del color, para que la primitiva siguiente
 * del lote la vea apagada (solo sin textura ni descarte alfa). */
typedef struct rsx_shadow_input {
    unsigned id, tid, oid, oid_test, trans_mask, opaque_mask;
    unsigned front, tdepth, equal, blend, single, consume, clear_alpha, extended;
} rsx_shadow_input;

/* Reparto de IDs en el stencil para un cuadro con sombras o igualdad, el
 * mismo que usa el PPE. Primera fase: una sola sombra (OID == sombra en el
 * bit 0, TID de la sombra 16, otros TID en los bits 1..3) o varias (OID en
 * los bits 0..1, TID en 2..4); `needs_back` si algún escritor deja la cara
 * trasera (empate frente/espalda), `needs_ids` si se agotan OID u TID. La
 * fase ampliada (rsx_shadow_layout_extend) reparte 4 bits entre los OID que
 * alguna sombra consulta y algún opaco escribe, y los TID; devuelve el motivo
 * del respaldo si no caben. */
typedef struct rsx_shadow_layout {
    uint8_t oid[64], tid[64], oid_test[64];
    unsigned trans_mask, opaque_mask, single, extended;
    unsigned needs_back, needs_ids, opaques, translucents;
} rsx_shadow_layout;

static inline unsigned rsx_poly_translucent(uint32_t attr, uint32_t texparam, const r3d_rstate *rs)
{
    unsigned fmt = (texparam >> 26) & 7u;
    return PA_MODE(attr) == 3 || PA_ALPHA(attr) < 31 ||
           (PA_MODE(attr) != 1 && (rs->d3 & D3_TEXTURE) && (fmt == 1 || fmt == 6));
}

static inline void rsx_shadow_layout_init(rsx_shadow_layout *l, const gx_frame *f, const r3d_rstate *rs, int general)
{
    unsigned shadow_id = 64, no = 1, nt = 1;
    memset(l, 0, sizeof *l);
    l->single = !general;
    for (unsigned i = 0; i < f->npolys; i++) {
        uint32_t a = f->poly[i].attr;
        if (PA_MODE(a) == 3 && PA_ID(a)) {
            if (shadow_id != 64 && shadow_id != PA_ID(a)) l->single = 0;
            shadow_id = PA_ID(a);
        }
    }
    if (shadow_id == 64) l->single = 0;
    memset(l->oid, 255, sizeof l->oid); memset(l->tid, 255, sizeof l->tid);
    memset(l->oid_test, 255, sizeof l->oid_test);
    if (l->single) {
        for (unsigned id = 0; id < 64; id++) l->oid[id] = id == shadow_id;
        l->tid[shadow_id] = 16;
    } else l->oid[AT_OID(rs->clear_attr)] = 0;
    l->trans_mask = l->single ? 30 : 28;
    for (unsigned i = 0; i < f->npolys; i++) {
        const gx_poly *p = &f->poly[i];
        unsigned a = PA_ALPHA(p->attr), m = PA_MODE(p->attr), id = PA_ID(p->attr);
        if (m == 3 && !id) continue;
        /* LESS solo es equivalente si ningún escritor deja AT_BACK.
         * No confundir el empate frontal/trasero con igualdad explícita. */
        if (!(p->flags & GX_POLY_FRONT) && ((m != 3 && a == 31) || (p->attr & PA_TDEPTH))) l->needs_back = 1;
        if (m != 3 && a == 31 && l->oid[id] == 255) l->oid[id] = no++;
        if (rsx_poly_translucent(p->attr, p->texparam, rs) && l->tid[id] == 255)
            l->tid[id] = nt++ << (l->single ? 1 : 2);
    }
    l->opaques = no; l->translucents = nt;
    l->needs_ids = no > 4 || nt > 8;
}

/* Bit 7: puerta Z; 6: cara; 5: máscara; 4: ocupado. Solo distinguir OID que
 * una sombra puede consultar y algún opaco (o el borrado) puede escribir. El
 * resto comparte cero. NULL si cabe; si no, el motivo del respaldo. */
static inline const char *rsx_shadow_layout_extend(rsx_shadow_layout *l, const gx_frame *f, const r3d_rstate *rs, int shadows)
{
    uint8_t writers[64] = {0}, readers[64] = {0};
    unsigned no = 1, nt = 1, bits = 0;
    writers[AT_OID(rs->clear_attr)] = 1;
    memset(l->oid, 0, sizeof l->oid); memset(l->tid, 255, sizeof l->tid);
    memset(l->oid_test, 255, sizeof l->oid_test);
    for (unsigned i = 0; i < f->npolys; i++) {
        uint32_t a = f->poly[i].attr;
        if (PA_MODE(a) == 3 && PA_ID(a)) readers[PA_ID(a)] = 1;
        if (PA_MODE(a) != 3 && PA_ALPHA(a) == 31) writers[PA_ID(a)] = 1;
    }
    for (unsigned id = 0; id < 64; id++) if (readers[id] && writers[id]) l->oid_test[id] = l->oid[id] = no++;
    while ((1u << bits) < no) bits++;
    if (bits > 3) return "sombras: capacidad de IDs excedida";
    l->opaque_mask = (1u << bits) - 1; l->trans_mask = 15 & ~l->opaque_mask;
    for (unsigned i = 0; i < f->npolys; i++) {
        uint32_t a = f->poly[i].attr;
        if (PA_MODE(a) == 3 && !PA_ID(a)) continue;
        if (rsx_poly_translucent(a, f->poly[i].texparam, rs) && l->tid[PA_ID(a)] == 255) l->tid[PA_ID(a)] = nt++ << bits;
    }
    if (nt > (16u >> bits)) return shadows ? "sombras: capacidad de IDs excedida" : "3D ampliado: más de 15 IDs translúcidos";
    l->opaques = no; l->translucents = nt;
    l->extended = 1;
    return NULL;
}

/* Máscara de sombra (modo 3, ID 0): marca el bit 5 donde FALLA la prueba de
 * profundidad; LEQUAL sobre una espalda si la máscara es frontal. */
static inline unsigned rsx_mask_passes(const rsx_shadow_input *c, rsx_pass out[2])
{
    unsigned n=0;
#define RP_ADD(t,r,cmp,wr,op,b,z,p,prog,zf,sf,rg) \
    (out[n++]=(rsx_pass){t,r,cmp,wr,op,b,z,p,prog,zf,sf,rg})
    if (!c->extended) RP_ADD(1,32,255,32,2,RP_NONE,RP_TEST|RP_LESS,0,RP_MASK,1,0,0);
    else if (c->front) {
        RP_ADD(3,32,64,32,2,RP_NONE,RP_TEST|RP_LESS,0,RP_MASK,1,0,0);
        RP_ADD(3,96,64,32,2,RP_NONE,RP_TEST,0,RP_MASK,1,0,0);
    } else RP_ADD(1,32,0,32,2,RP_NONE,RP_TEST|RP_LESS,0,RP_MASK,1,0,0);
#undef RP_ADD
    return n;
}

/* Sombra (modo 3, ID distinto de 0): solo donde la máscara marcó y el OID no
 * es el suyo; consume la máscara aunque falle; se mezcla como translúcida. */
static inline unsigned rsx_shadow_passes(const rsx_shadow_input *c, rsx_pass out[12])
{
    unsigned n=0;
    const unsigned zw=RP_TEST|RP_LESS|(c->tdepth ? RP_WRITE : 0);
    const unsigned mix=c->blend ? RP_BLEND : RP_OFF, mixpass=c->blend ? 1 : 2;
#define RP_ADD(t,r,cmp,wr,op,b,z,p,prog,zf,sf,rg) \
    (out[n++]=(rsx_pass){t,r,cmp,wr,op,b,z,p,prog,zf,sf,rg})
    if (c->extended) {
        const unsigned back=c->front ? 0 : 64;
        RP_ADD(1,0,0,128,0,RP_NONE,0,0,RP_MASK,0,0,0);
        if (c->equal) {
            RP_ADD(1,128,0,128,0,RP_NONE,RP_TEST,0,RP_RANGE,0,0,-1);
            RP_ADD(1,0,0,128,2,RP_NONE,RP_TEST|RP_GEQUAL,0,RP_RANGE,2,0,1);
        } else if (c->front) {
            RP_ADD(3,128,64,128,0,RP_NONE,RP_TEST|RP_LESS,0,RP_MASK,0,0,0);
            RP_ADD(3,192,64,128,0,RP_NONE,RP_TEST,0,RP_MASK,0,0,0);
        } else RP_ADD(1,128,0,128,0,RP_NONE,RP_TEST|RP_LESS,0,RP_MASK,0,0,0);
        RP_ADD(3,0,32,128,0,RP_NONE,0,0,RP_MASK,0,0,0);
        if (c->oid_test!=255) RP_ADD(3,c->oid_test,c->opaque_mask,128,0,RP_NONE,0,0,RP_MASK,0,0,0);
        RP_ADD(3,c->tid,c->trans_mask,128,0,RP_NONE,0,0,RP_MASK,0,0,0);
        {
            const unsigned wz=c->trans_mask|(c->tdepth ? 64 : 0);
            const unsigned dz=RP_TEST|RP_ALWAYS|(c->tdepth ? RP_WRITE : 0);
            RP_ADD(3,128|16|back|c->tid,128|16,wz,0,mix,dz,mixpass,RP_COLOR,0,0,0);
            RP_ADD(3,128|back|c->tid,128|16,wz,0,RP_OFF,dz,2,RP_COLOR,0,0,0);
            RP_ADD(3,16|c->tid,c->trans_mask,16,0,RP_NONE,0,2,RP_COLOR,0,0,0);
        }
        RP_ADD(1,0,0,32,0,RP_NONE,0,0,RP_MASK,0,0,0);
    } else if (c->single) {
        /* Bits 7/5/4/0: ocupado, máscara, último ID de sombra, OID igual a
         * sombra. La comparación rechaza ambos IDs; invertir publica el TID
         * sin exigirlo antes de dibujar. Un borrado con alfa ocupa toda la
         * pantalla: ninguna pasada admitida vuelve a dejar un destino vacío. */
        RP_ADD(3,160,177,c->consume ? 48 : 16,1,mix,zw,mixpass,RP_COLOR,c->consume ? 2 : 0,0,0);
        if (!c->consume || !c->clear_alpha)
            RP_ADD(3,32,177,c->consume ? 176 : 144,1,RP_OFF,zw,2,RP_COLOR,c->consume ? 2 : 0,0,0);
        RP_ADD(1,0,0,32,0,RP_NONE,0,0,RP_MASK,0,0,0);
    } else {
        /* Consumir la máscara aun si falla Z, ID o alfa. El bit 6 solo vive
         * durante este polígono; OID nunca se pierde. */
        RP_ADD(3,96,32,64,0,RP_NONE,0,0,RP_MASK,0,0,0);
        if (c->oid!=255) RP_ADD(3,c->oid,3,64,0,RP_NONE,0,0,RP_MASK,0,0,0);
        RP_ADD(3,c->tid,c->trans_mask,64,0,RP_NONE,0,0,RP_MASK,0,0,0);
        RP_ADD(3,c->tid|192,192,c->trans_mask|128,0,mix,zw,mixpass,RP_COLOR,0,0,0);
        RP_ADD(3,c->tid|64,192,c->trans_mask,0,RP_OFF,zw,2,RP_COLOR,0,0,0);
        RP_ADD(3,c->tid|128,c->trans_mask,128,0,RP_NONE,0,2,RP_COLOR,0,0,0);
        RP_ADD(1,0,0,96,0,RP_NONE,0,0,RP_MASK,0,0,0);
    }
#undef RP_ADD
    return n;
}

/* Polígonos de color (modo 0..2) con sombras en el cuadro, sin ampliar: el
 * opaco publica OID y ocupado; el translúcido marca el ID repetido (bit 6),
 * mezcla sobre ocupado, reemplaza en vacío y publica TID y ocupado. */
static inline unsigned rsx_shadow_color_passes(const rsx_shadow_input *c, unsigned trans, rsx_pass out[6])
{
    unsigned n=0;
    const unsigned zw=RP_TEST|RP_LESS|(c->tdepth ? RP_WRITE : 0);
#define RP_ADD(t,r,cmp,wr,op,b,z,p,prog,zf,sf,rg) \
    (out[n++]=(rsx_pass){t,r,cmp,wr,op,b,z,p,prog,zf,sf,rg})
    RP_ADD(1,c->oid|128,255,159,0,RP_OFF,RP_TEST|RP_LESS|RP_WRITE,0,RP_COLOR,0,0,0);
    if (trans) {
        RP_ADD(3,c->tid,(c->single && c->tid==16) ? 16 : c->trans_mask,64,1,RP_NONE,RP_TEST|RP_LESS,2,RP_COLOR,0,0,0);
        RP_ADD(3,c->tid|128,192,c->trans_mask|128,0,c->blend ? RP_BLEND : RP_OFF,zw,c->blend ? 1 : 2,RP_COLOR,0,0,0);
        RP_ADD(3,c->tid,128,c->trans_mask,0,RP_OFF,zw,2,RP_COLOR,0,0,0);
        RP_ADD(3,c->tid|128,c->trans_mask,128,0,RP_NONE,0,2,RP_COLOR,0,0,0);
        RP_ADD(3,64,64,64,1,RP_NONE,0,2,RP_COLOR,0,0,0);
    }
#undef RP_ADD
    return n;
}
#endif
