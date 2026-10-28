/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "gpu3d/rsx_passes.h"
#include "gpu3d/r3d_px.h"

typedef struct sample {
    uint32_t color, depth, fogdepth, fog;
    unsigned stencil;
} sample;

/* Intérprete de estados fijos: prueba stencil, descarte del fragmento,
 * profundidad, mezcla y escritura de stencil. Consume el plan de producción. */
static sample execute(sample s, const rsx_pass_input *in, unsigned alpha,
                      unsigned alpha_ref, unsigned depth, unsigned fog, unsigned w)
{
    rsx_pass plan[9]; unsigned n=rsx_color_passes(in,plan);
    for (unsigned i=0;i<n;i++) {
        const rsx_pass *p=&plan[i];
        unsigned eq=(s.stencil&p->compare)==(p->ref&p->compare);
        if ((p->test==2 && eq) || (p->test==3 && !eq)) continue;
        if (p->program==RP_COLOR &&
            (alpha<=alpha_ref || (alpha<31)!=(p->pass!=0))) continue;
        unsigned z=depth+p->range*(w ? 255 : 512);
        unsigned ok=!(p->depth&RP_TEST) || (p->depth&RP_ALWAYS) ||
            ((p->depth&RP_GEQUAL) ? z>=s.depth : (p->depth&RP_LESS) ? z<s.depth : z<=s.depth);
        if (!ok) {
            if (p->zfail) s.stencil=(s.stencil&~p->write)|((p->zfail==2 ? 0 : p->ref)&p->write);
            continue;
        }
        if (p->blend!=RP_NONE) {
            if (in->attributes) {
                if (!p->pass || in->tdepth) s.fogdepth=depth>>9;
                s.fog=p->pass ? (s.fog&fog) : fog;
            } else {
                uint32_t c=R3D_PX(53,17,29,alpha);
                if (p->blend==RP_BLEND) {
                    unsigned a=alpha+1, b=31-alpha, da=R3D_PX_A(s.color);
                    c=R3D_PX((53*a+R3D_PX_R(s.color)*b)>>5,
                             (17*a+R3D_PX_G(s.color)*b)>>5,
                             (29*a+R3D_PX_B(s.color)*b)>>5, alpha>da ? alpha : da);
                }
                s.color=c;
            }
        }
        if (p->depth&RP_WRITE) s.depth=z;
        unsigned v=p->op==2 ? s.stencil : p->op==1 ? ~s.stencil : p->ref;
        s.stencil=(s.stencil&~p->write)|(v&p->write);
    }
    return s;
}

void test_rsx_passes(void)
{
    unsigned cases=0;
    /* Barrido de estado previo válido y todos los controles independientes.
     * Los desplazamientos incluyen ambos bordes de igualdad y sus vecinos. */
    static const int delta[]={-513,-512,-256,-255,-1,0,1,255,256,512,513};
    static const unsigned alphas[]={0,1,16,30,31};
    for (unsigned bits=0;bits<8192;bits++) {
        rsx_pass_input c={0};
        c.extended=bits&1; c.front=(bits>>1)&1; c.equal=(bits>>2)&1;
        c.blend=(bits>>3)&1; c.attributes=(bits>>4)&1; c.tdepth=(bits>>5)&1;
        unsigned empty=(bits>>6)&1, dst_trans=(bits>>7)&1, repeated=(bits>>8)&1;
        unsigned dst_back=(bits>>9)&1, w=(bits>>10)&1, poly_trans=(bits>>11)&1;
        if ((!c.extended && (c.equal || dst_back)) || (empty && dst_trans)) continue;
        c.prior_back=dst_back; c.occupied=!empty;
        c.id=repeated ? 3 : 4; c.tid=repeated ? 4 : 8; c.oid=1; c.trans_mask=12;
        c.opaque=!poly_trans; c.trans=!(bits&4096);
        if (!c.trans && poly_trans) continue;
        for (unsigned a=0;a<5;a++) for (unsigned d=0;d<11;d++)
        for (unsigned flags=0;flags<32;flags++) {
            unsigned alpha=alphas[a];
            if ((poly_trans && alpha==31) || (!c.trans && alpha!=0 && alpha!=31)) continue;
            c.occupied=!empty && !(flags&8); c.prior_back=dst_back || !!(flags&16);
            unsigned fog=flags&1, oldfog=(flags>>1)&1, alpha_ref=(flags&4) ? 16 : 0;
            unsigned z=10000+delta[d], da=empty ? 0 : 19;
            sample s={R3D_PX(7,43,59,da),10000,10000>>9,oldfog,0};
            s.stencil=c.extended ? (32|(dst_back ? 64 : 0)|(empty ? 0 : 16)|(dst_trans ? 4 : 0)|2) :
                (empty ? 255 : dst_trans ? 3 : 64);
            sample got=execute(s,&c,alpha,alpha_ref,z,fog,w), want=s;
            r3d_rstate rs={0}; rs.wbuf=w;
            r3d_poly p={0}; p.front=c.front; p.attr=c.equal ? PA_DEQUAL : 0;
            unsigned accepted=alpha>alpha_ref && r3d_depth_ok(&p,&rs,z,s.depth,dst_back ? AT_BACK : 0) &&
                !(alpha<31 && dst_trans && repeated);
            if (accepted) {
                if (alpha==31 || c.tdepth) want.depth=z;
                if (c.attributes) {
                    if (alpha==31 || c.tdepth) want.fogdepth=z>>9;
                    want.fog=alpha==31 ? fog : oldfog&fog;
                } else if (alpha<31 && c.blend && da) {
                    unsigned a=alpha+1,b=31-alpha;
                    want.color=R3D_PX((53*a+7*b)>>5,(17*a+43*b)>>5,(29*a+59*b)>>5,alpha>da ? alpha : da);
                } else want.color=R3D_PX(53,17,29,alpha);
                if (c.extended) {
                    if (alpha==31) want.stencil=32|16|c.oid|(c.front ? 0 : 64);
                    else want.stencil=(s.stencil&~(12|16|(c.tdepth ? 64 : 0)))|16|c.tid|
                        ((c.tdepth && !c.front) ? 64 : 0);
                } else want.stencil=alpha==31 ? 64 : c.id;
            }
            /* Puerta temporal bit 7 no es estado persistente del píxel. */
            unsigned sm=c.extended ? 127 : 255;
            if (got.color!=want.color || got.depth!=want.depth || got.fog!=want.fog ||
                got.fogdepth!=want.fogdepth || (got.stencil&sm)!=(want.stencil&sm)) {
                printf("pasadas: bits %u alfa %u delta %d flags %u\n",bits,alpha,delta[d],flags);
                printf("color %08x/%08x Z %u/%u niebla %u:%u/%u:%u stencil %x/%x\n",
                       got.color,want.color,got.depth,want.depth,got.fogdepth,got.fog,
                       want.fogdepth,want.fog,got.stencil,want.stencil);
                CHECK(0); return;
            }
            cases++;
        }
    }
    CHECK(cases>100000);
    /* Sin mezcla, atributos o borrado ocupado: una pasada translúcida.
     * Borrado vacío con mezcla: reemplazo antes de mezcla, dos pasadas. */
    rsx_pass_input c={0}; rsx_pass plan[9];
    c.id=63; c.trans=1; c.blend=1;
    CHECK_EQ(rsx_color_passes(&c,plan),2);
    c.attributes=1; CHECK_EQ(rsx_color_passes(&c,plan),1);
    c.attributes=0; c.occupied=1; CHECK_EQ(rsx_color_passes(&c,plan),1);
    c.extended=1; c.trans=0; c.opaque=1; c.front=1;
    CHECK_EQ(rsx_color_passes(&c,plan),1);
    c.prior_back=1; CHECK_EQ(rsx_color_passes(&c,plan),2);
    printf("RSX pasadas: %u estados contra las reglas escalares, 0 diferencias\n",cases);
}
