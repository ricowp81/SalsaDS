/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "gpu3d/rsx_passes.h"
#include "gpu3d/r3d_px.h"

typedef struct sample {
    uint32_t color, depth, fogdepth, fog;
    unsigned stencil;
} sample;

/* Intérprete de estados fijos: descarte del fragmento, stencil, profundidad,
 * mezcla y escritura de stencil, en el orden del RSX (un fragmento descartado
 * por el programa no toca stencil ni profundidad). Ejecuta una pasada. */
static sample execute_pass(sample s, const rsx_pass *p, const rsx_pass_input *in, unsigned alpha,
                           unsigned alpha_ref, unsigned depth, unsigned fog, unsigned w)
{
    if (p->program==RP_COLOR && (alpha<=alpha_ref || (alpha<31)!=(p->pass!=0))) return s;
    unsigned eq=(s.stencil&p->compare)==(p->ref&p->compare);
    if ((p->test==2 && eq) || (p->test==3 && !eq)) {
        if (p->sfail) s.stencil=(s.stencil&~p->write)|((p->sfail==2 ? 0 : p->ref)&p->write);
        return s;
    }
    unsigned z=depth+p->range*(w ? 255 : 512);
    unsigned ok=!(p->depth&RP_TEST) || (p->depth&RP_ALWAYS) ||
        ((p->depth&RP_EQUAL) ? z==s.depth : (p->depth&RP_GEQUAL) ? z>=s.depth :
         (p->depth&RP_LESS) ? z<s.depth : z<=s.depth);
    if (!ok) {
        if (p->zfail) s.stencil=(s.stencil&~p->write)|((p->zfail==2 ? 0 : p->ref)&p->write);
        return s;
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
    return s;
}
static sample execute(sample s, const rsx_pass_input *in, unsigned alpha,
                      unsigned alpha_ref, unsigned depth, unsigned fog, unsigned w, int compact)
{
    rsx_pass plan[9]; unsigned n=rsx_color_passes_mode(in,plan,compact);
    for (unsigned i=0;i<n;i++) s=execute_pass(s,&plan[i],in,alpha,alpha_ref,depth,fog,w);
    return s;
}

static unsigned sweep(int compact, unsigned mask)
{
    /* mask: bits de TID (12: dos bits con OID; 15: cuatro bits, sin sombras) */
    const unsigned dst_tid=mask==15 ? 13 : 4, other_tid=mask==15 ? 9 : 8;
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
        c.id=repeated ? 3 : 4; c.tid=repeated ? dst_tid : other_tid; c.oid=mask==15 ? 0 : 1; c.trans_mask=mask;
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
            /* El bit 7 (puerta temporal) empieza en cualquier estado. */
            s.stencil=c.extended ? ((flags&2) ? 128 : 0)|32|(dst_back ? 64 : 0)|(empty ? 0 : 16)|(dst_trans ? dst_tid : 0)|(mask==15 ? 0 : 2) :
                (empty ? 255 : dst_trans ? 3 : 64);
            sample got=execute(s,&c,alpha,alpha_ref,z,fog,w,compact), want=s;
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
                    else want.stencil=(s.stencil&~(mask|16|(c.tdepth ? 64 : 0)))|16|c.tid|
                        ((c.tdepth && !c.front) ? 64 : 0);
                } else want.stencil=alpha==31 ? 64 : c.id;
            }
            /* Puerta temporal bit 7 no es estado persistente del píxel. */
            unsigned sm=c.extended ? 127 : 255;
            if (got.color!=want.color || got.depth!=want.depth || got.fog!=want.fog ||
                got.fogdepth!=want.fogdepth || (got.stencil&sm)!=(want.stencil&sm)) {
                printf("pasadas (%s): bits %u alfa %u delta %d flags %u\n",compact ? "compacto" : "ronda 6",bits,alpha,delta[d],flags);
                printf("color %08x/%08x Z %u/%u niebla %u:%u/%u:%u stencil %x/%x\n",
                       got.color,want.color,got.depth,want.depth,got.fogdepth,got.fog,
                       want.fogdepth,want.fog,got.stencil,want.stencil);
                CHECK(0); return cases;
            }
            cases++;
        }
    }
    return cases;
}

/* Lotes de opacos frontales en 3D ampliado: las dos pasadas (espaldas con
 * LEQUAL, frentes con LESS) se emiten por lote, no por polígono. El resultado
 * debe ser el de la DS polígono a polígono, con cualquier destino previo y
 * cualquier mezcla de empates dentro del lote. */
static unsigned batches(void)
{
    unsigned cases=0;
    static const int delta[]={-2,-1,0,1,2};
    for (unsigned n=1;n<=4;n++) for (unsigned dst_back=0;dst_back<2;dst_back++)
    for (unsigned seq=0;seq<625;seq++) {
        unsigned zs[4], v=seq;
        for (unsigned i=0;i<n;i++) { zs[i]=1000+delta[v%5]; v/=5; }
        /* Referencia DS: cada polígono frontal con «menor» o empate sobre espalda. */
        unsigned depth=1000, back=dst_back, owner=0;
        for (unsigned i=0;i<n;i++) {
            if (zs[i]<depth || (back && zs[i]==depth)) { depth=zs[i]; back=0; owner=i+1; }
        }
        /* Mismo lote (misma clave); el OID distingue al dueño del píxel. */
        rsx_pass plans[4][9]; unsigned np=0;
        rsx_pass_input c={0};
        c.extended=1; c.front=1; c.opaque=1; c.prior_back=1; c.trans_mask=12;
        for (unsigned i=0;i<n;i++) { c.oid=i+1; np=rsx_color_passes(&c,plans[i]); CHECK_EQ(np,2); }
        /* Lote: pasada 0 de todos los polígonos y después la pasada 1. */
        sample s={R3D_PX(0,0,0,0),1000,0,0,32|(dst_back ? 64 : 0)|16};
        for (unsigned q=0;q<np;q++) for (unsigned i=0;i<n;i++)
            s=execute_pass(s,&plans[i][q],&c,31,0,zs[i],0,0);
        unsigned got_owner=s.stencil&15;
        if (got_owner!=owner || s.depth!=depth || ((s.stencil>>6)&1)!=back) {
            printf("lote n=%u espalda %u zs %u %u %u %u: dueño %u/%u Z %u/%u espalda %u/%u\n",n,dst_back,
                   zs[0],n>1 ? zs[1] : 0,n>2 ? zs[2] : 0,n>3 ? zs[3] : 0,got_owner,owner,s.depth,depth,(s.stencil>>6)&1,back);
            CHECK(0); return cases;
        }
        cases++;
    }
    return cases;
}

void test_rsx_passes(void)
{
    unsigned compact=sweep(1,12), previous=sweep(0,12), wide=sweep(1,15);
    CHECK(compact>100000 && previous==compact && wide==compact);
    unsigned lotes=batches();
    CHECK(lotes>2000);
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
    /* Translúcido ampliado: puerta (dos pasadas con espalda previa) y color;
     * la ronda 6 necesitaba borrado y rechazo de ID aparte. */
    c.trans=1; c.opaque=0; c.tid=8; c.trans_mask=12;
    CHECK_EQ(rsx_color_passes(&c,plan),3);
    CHECK_EQ(rsx_color_passes_mode(&c,plan,0),5);
    c.prior_back=0; CHECK_EQ(rsx_color_passes(&c,plan),2);
    c.equal=1; CHECK_EQ(rsx_color_passes(&c,plan),3);
    printf("RSX pasadas: %u estados contra las reglas escalares por plan (compacto y ronda 6), %u lotes frontales, 0 diferencias\n",compact,lotes);
}
