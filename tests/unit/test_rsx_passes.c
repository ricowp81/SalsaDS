/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "gpu3d/rsx_passes.h"
#include "gpu3d/rsx_modes.h"
#include "gpu3d/r3d_px.h"
#include "gpu3d/r3d_core.h"
#include "gpu3d/rsx_edge.h"
#include "gpu3d/rsx_edge_test.h"

typedef struct sample {
    uint32_t color, depth, fogdepth, fog;
    unsigned stencil;
} sample;
/* Lo que el programa de color decide por fragmento: alfa (y su referencia),
 * profundidad, bandera de niebla, W o Z, sombra (siempre translúcida) y color. */
typedef struct frag { unsigned alpha, alpha_ref, depth, fog, w, shadow; uint32_t rgb; } frag;

/* Intérprete de estados fijos: descarte del fragmento, stencil, profundidad,
 * mezcla y escritura de stencil, en el orden del RSX (un fragmento descartado
 * por el programa no toca stencil ni profundidad). Ejecuta una pasada. */
static sample execute_frag(sample s, const rsx_pass *p, unsigned attributes, unsigned tdepth, const frag *f)
{
    const unsigned alpha=f->alpha, depth=f->depth, fog=f->fog, w=f->w;
    if (p->program==RP_COLOR && (alpha<=f->alpha_ref || (f->shadow || alpha<31)!=(p->pass!=0))) return s;
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
        if (attributes==2) {
            /* Atributos de bordes: RGB24 cambia con cada escritor de Z;
             * el byte OID/arista solo se reemplaza con fragmentos opacos.
             * f->fog transporta aquí ese byte, separado de la niebla. */
            if (!p->pass || tdepth) s.fogdepth=depth;
            if (!p->pass) s.fog=fog;
        } else if (attributes) {
            if (!p->pass || tdepth) s.fogdepth=depth>>9;
            s.fog=p->pass ? (s.fog&fog) : fog;
        } else {
            uint32_t c=f->rgb|((uint32_t)alpha<<24);
            if (p->blend==RP_BLEND) {
                unsigned a=alpha+1, b=31-alpha, da=R3D_PX_A(s.color);
                c=R3D_PX((R3D_PX_R(f->rgb)*a+R3D_PX_R(s.color)*b)>>5,
                         (R3D_PX_G(f->rgb)*a+R3D_PX_G(s.color)*b)>>5,
                         (R3D_PX_B(f->rgb)*a+R3D_PX_B(s.color)*b)>>5, alpha>da ? alpha : da);
            }
            s.color=c;
        }
    }
    if (p->depth&RP_WRITE) s.depth=z;
    unsigned v=p->op==2 ? s.stencil : p->op==1 ? ~s.stencil : p->ref;
    s.stencil=(s.stencil&~p->write)|(v&p->write);
    return s;
}
static sample execute_pass(sample s, const rsx_pass *p, const rsx_pass_input *in, unsigned alpha,
                           unsigned alpha_ref, unsigned depth, unsigned fog, unsigned w)
{
    frag f={alpha,alpha_ref,depth,fog,w,0,R3D_PX(53,17,29,0)};
    return execute_frag(s,p,in->attributes,in->tdepth,&f);
}
static sample execute(sample s, const rsx_pass_input *in, unsigned alpha,
                      unsigned alpha_ref, unsigned depth, unsigned fog, unsigned w, int compact)
{
    rsx_pass plan[9]; unsigned n=rsx_color_passes_mode(in,plan,compact);
    for (unsigned i=0;i<n;i++) s=execute_pass(s,&plan[i],in,alpha,alpha_ref,depth,fog,w);
    return s;
}

/* ---- Sombras: secuencias de polígonos sobre un píxel --------------------------------------------- */
/* Referencia escalar de la DS para un píxel (r3d_draw_row en r3d_px.h, sin
 * textura): máscara, sombra, opaco y translúcido con sus atributos. */
typedef struct dspx { uint32_t col; unsigned dep, oid, tid, trans, fog, stencil, back; } dspx;
typedef struct dspoly { unsigned shadow, id, alpha, front, tdepth, fog, d; uint32_t rgb; } dspoly;
static void ds_apply(dspx *x, const dspoly *p, unsigned blend, unsigned alpha_ref)
{
    int ok=(p->front && x->back) ? p->d<=x->dep : p->d<x->dep;
    if (p->shadow && !p->id) { if (!ok) x->stencil=1; return; }
    if (p->shadow) { if (!x->stencil) return; x->stencil=0; if (x->oid==p->id) return; }
    if (!ok) return;
    unsigned a=p->alpha;
    if (a==0 || a<=alpha_ref) return;
    if (a==31 && !p->shadow) {
        x->col=p->rgb|(a<<24); x->dep=p->d; x->oid=p->id; x->trans=0; x->tid=0;
        x->fog=p->fog; x->back=!p->front;
        return;
    }
    if (x->trans && x->tid==p->id) return;
    uint32_t c=p->rgb|(a<<24); unsigned da=R3D_PX_A(x->col);
    if (blend && da) {
        unsigned ia=31u-a, sa=a+1u;
        c=R3D_PX((R3D_PX_R(c)*sa+R3D_PX_R(x->col)*ia)>>5,(R3D_PX_G(c)*sa+R3D_PX_G(x->col)*ia)>>5,
                 (R3D_PX_B(c)*sa+R3D_PX_B(x->col)*ia)>>5,a>da ? a : da);
    }
    x->col=c;
    if (p->tdepth) { x->dep=p->d; x->back=!p->front; }
    x->trans=1; x->tid=p->id; x->fog=x->fog && p->fog;
}

/* Una escena: el reparto de IDs del PPE (rsx_shadow_layout), el stencil
 * inicial del borrado y los planes de cada polígono en orden, contra la
 * referencia. 1 si coincide, 0 si difiere, -1 si el reparto no cabe. */
static gx_frame shadow_frame;
static int shadow_scene(const dspoly *polys, unsigned n, unsigned w, unsigned blend, unsigned alpha_ref,
                        unsigned clear_alpha, unsigned clear_fog, unsigned attributes, int general, unsigned *detail)
{
    gx_frame *f=&shadow_frame;
    memset(&f->regs,0,sizeof f->regs); f->npolys=n; f->nverts=0; f->swap_params=w ? 3 : 1;
    f->regs.clear_depth=0x7fff;
    f->regs.clear_color=(5u<<24)|(clear_alpha<<16)|(clear_fog ? 0x8000u : 0u)|0x4210u;
    f->regs.disp3dcnt=(blend ? D3_BLEND : 0)|(alpha_ref ? D3_ALPHATEST : 0);
    f->regs.alpha_test_ref=alpha_ref;
    unsigned shadows=0;
    for (unsigned i=0;i<n;i++) {
        gx_poly *g=&f->poly[i]; memset(g,0,sizeof *g);
        g->attr=(polys[i].id<<24)|(polys[i].alpha<<16)|(polys[i].shadow ? 3u<<4 : 0)|
                (polys[i].tdepth ? PA_TDEPTH : 0)|(polys[i].fog ? PA_FOG : 0);
        g->flags=polys[i].front ? GX_POLY_FRONT : 0; g->nverts=3;
        shadows|=polys[i].shadow;
    }
    r3d_rstate rs; r3d_rstate_init(&rs,f,0);
    rsx_shadow_layout l; rsx_shadow_layout_init(&l,f,&rs,general);
    if (l.translucents>8) return -1;
    if (l.needs_back || l.needs_ids) { if (rsx_shadow_layout_extend(&l,f,&rs,shadows)) return -1; }
    const unsigned ca=clear_alpha!=0, clear_oid=5;
    sample s={rs.clear_px,rs.clear_depth,rs.clear_depth>>9,clear_fog,
              l.extended ? ((ca ? 16u : 0u)|l.oid[clear_oid]) : ((ca ? 128u : 0u)|l.oid[clear_oid])};
    dspx x={rs.clear_px,rs.clear_depth,clear_oid,0,0,clear_fog,0,0};
    unsigned prior_back=0;
    for (unsigned i=0;i<n;i++) {
        const dspoly *q=&polys[i];
        rsx_pass plan[12]; unsigned np;
        frag fr={q->alpha,alpha_ref,q->d,q->fog,w,q->shadow,q->rgb};
        if (q->shadow || !l.extended) {
            rsx_shadow_input in={q->id,l.tid[q->id],l.oid[q->id],l.extended ? l.oid_test[q->id] : 255,
                l.trans_mask,l.opaque_mask,q->front,q->tdepth,0,blend,l.single,
                l.single && q->alpha>alpha_ref,ca,l.extended};
            if (!q->shadow) np=rsx_shadow_color_passes(&in,q->alpha<31,plan);
            else np=q->id ? rsx_shadow_passes(&in,plan) : rsx_mask_passes(&in,plan);
        } else {
            rsx_pass_input in={q->id,l.tid[q->id],l.oid[q->id],l.trans_mask,q->front,q->alpha<31,q->alpha==31,
                q->tdepth,0,1,blend,ca,attributes,prior_back};
            np=rsx_color_passes(&in,plan);
        }
        prior_back|=!q->front && (q->alpha==31 || q->tdepth);
        for (unsigned k=0;k<np;k++) s=execute_frag(s,&plan[k],attributes,q->tdepth,&fr);
        ds_apply(&x,q,blend,alpha_ref);
    }
    *detail=l.extended ? 2 : l.single ? 0 : 1;
    /* La pasada de atributos no escribe color: profundidad >>9 y bandera. */
    if (s.depth!=x.dep) return 0;
    if (!attributes && s.color!=x.col) return 0;
    if (attributes && (s.fog!=x.fog || s.fogdepth!=(x.dep>>9))) return 0;
    return 1;
}

static unsigned shadows_sweep(void)
{
    static const unsigned d1[]={1000,1001}, d2[]={999,1000}, alphas[]={13,31};
    unsigned cases=0, layouts[3]={0,0,0}, skipped=0;
    for (unsigned g=0;g<(1u<<7);g++) {
        const unsigned w=g&1, blend=(g>>1)&1, ca=(g>>2)&1 ? 19 : 0, cfog=(g>>3)&1, attributes=(g>>4)&1;
        const unsigned alpha_ref=(g>>5)&1 ? 13 : 0, general=(g>>6)&1;
        for (unsigned p0=0;p0<4;p0++) for (unsigned p1=0;p1<4;p1++) for (unsigned p2=0;p2<64;p2++) for (unsigned p3=0;p3<4;p3++) {
            dspoly polys[6]; unsigned n=0;
            polys[n++]=(dspoly){0,1,31,!(p0&1),0,(p0>>1)&1,1000,R3D_PX(63,63,63,0)};
            polys[n++]=(dspoly){1,0,13,!(p1&1),0,0,d1[(p1>>1)&1],0};
            polys[n++]=(dspoly){1,(p2&1) ? 1 : 2,alphas[(p2>>1)&1],!((p2>>2)&1),(p2>>3)&1,(p2>>4)&1,d2[(p2>>5)&1],R3D_PX(0,0,0,0)};
            if (p3==1) polys[n++]=(dspoly){0,2,15,1,(p2>>3)&1,1,998,R3D_PX(0,63,0,0)};
            if (p3>=2) {
                polys[n++]=(dspoly){1,0,13,1,0,0,999,0};
                polys[n++]=(dspoly){1,p3==2 ? 2 : 3,13,1,0,(p2>>4)&1,999,R3D_PX(0,0,63,0)};
            }
            unsigned detail=0;
            int r=shadow_scene(polys,n,w,blend,alpha_ref,ca,cfog,attributes,general,&detail);
            if (r<0) { skipped++; continue; }
            if (!r) {
                printf("sombras: globales %u polígonos %u %u %u %u: reparto %u difiere de la referencia\n",g,p0,p1,p2,p3,detail);
                CHECK(0); return cases;
            }
            layouts[detail]++; cases++;
        }
    }
    printf("RSX sombras: %u escenas de hasta 5 polígonos (una sombra %u, varias %u, ampliado %u; %u repartos sin capacidad), con atributos de niebla, 0 diferencias\n",
           cases,layouts[0],layouts[1],layouts[2],skipped);
    CHECK(layouts[0]>1000 && layouts[1]>1000 && layouts[2]>1000);
    return cases;
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

/* Metadatos de bordes: vecinos, igualdad exacta de 24 bits y alfa intacta.
 * La referencia recibe los atributos DS; la pasada recibe solo RGB24/OID6
 * y la candidatura geométrica. Las banderas TID/niebla no son el OID. */
static void edge_pixels(void)
{
    static uint32_t col[256], want[256], dep[3][256], att[3][256];
    static const uint32_t depths[]={0,1,255,256,511,512,65535,65536,0x600000,0xfffffe,0xffffff};
    r3d_rstate rs={0};
    uint32_t seed=1;
    unsigned different=0, marked=0, unchanged=0;
    for (unsigned k=0;k<8;k++) rs.edge[k]=r3d_555((k*3967u)&0x7fff,31);
    for (unsigned test=0;test<512;test++) {
        rs.clear_attr=test&63; rs.clear_depth=depths[test%11]; rs.d3=(test&1) ? D3_AA : 0;
        for (unsigned y=0;y<3;y++) for (unsigned x=0;x<256;x++) {
            seed=seed*1664525u+1013904223u;
            att[y][x]=(seed>>9)&0x1ffffu;
            dep[y][x]=depths[(seed>>26)%11];
            if (y==1) col[x]=R3D_PX((seed>>18)&63,(seed>>12)&63,(seed>>6)&63,seed&31);
        }
        memcpy(want,col,sizeof want);
        const uint32_t *du=(test&2) ? NULL : dep[0], *au=(test&2) ? NULL : att[0];
        const uint32_t *dd=(test&4) ? NULL : dep[2], *ad=(test&4) ? NULL : att[2];
        r3d_edge_row(&rs,want,dep[1],att[1],du,au,dd,ad);
        uint32_t clear=rsx_edge_encode(rs.clear_depth,rs.clear_attr);
        for (unsigned x=0;x<256;x++) {
            uint32_t center=rsx_edge_encode(dep[1][x],att[1][x]);
            uint32_t neighbors[4]={x ? rsx_edge_encode(dep[1][x-1],att[1][x-1]) : clear,
                x<255 ? rsx_edge_encode(dep[1][x+1],att[1][x+1]) : clear,
                du ? rsx_edge_encode(du[x],au[x]) : clear,dd ? rsx_edge_encode(dd[x],ad[x]) : clear};
            uint32_t got=rsx_edge_apply(&rs,col[x],center,neighbors);
            different+=got!=want[x]; marked+=got!=col[x]; unchanged+=got==col[x];
            CHECK_EQ(rsx_edge_depth(center),dep[1][x]);
            CHECK_EQ(rsx_edge_meta_id(center),AT_OID(att[1][x]));
            CHECK_EQ(R3D_PX_A(got),R3D_PX_A(col[x]));
        }
    }
    CHECK_EQ(different,0); CHECK(marked>1000 && unchanged>1000);
    /* Los nueve bits bajos cuentan: compartir z>>9 no es igualdad Z/W. */
    for (unsigned id=0;id<64;id++) for (unsigned alpha=0;alpha<32;alpha++)
    for (unsigned candidate=0;candidate<2;candidate++) for (unsigned direction=0;direction<4;direction++)
    for (int delta=-1;delta<=1;delta++) {
        uint32_t center=rsx_edge_encode(0x600100,id|(candidate ? AT_EDGE : 0));
        uint32_t neighbors[4]={center,center,center,center};
        neighbors[direction]=rsx_edge_encode(0x600100+delta,(id+1)&63);
        uint32_t color=R3D_PX(2,4,6,alpha), got=rsx_edge_apply(&rs,color,center,neighbors);
        uint32_t expected=candidate && delta>0 ? (rs.edge[id>>3]&0xffffffu)|(alpha<<24) : color;
        CHECK_EQ(got,expected);
    }
    /* Un translúcido no publica su TID como OID, ni crea AT_EDGE. Su alfa
     * auxiliar 255 con MIN conserva exactamente los siete bits del opaco;
     * la niebla usa su propia pasada de atributos y no comparte este byte. */
    for (unsigned oid=0;oid<64;oid++) for (unsigned edge=0;edge<2;edge++)
    for (unsigned tid=0;tid<64;tid++) for (unsigned fog=0;fog<2;fog++) {
        uint32_t a=oid|(edge ? AT_EDGE : 0)|AT_TRANS|(tid<<6)|(fog ? AT_FOG : 0);
        uint32_t value=rsx_edge_encode(0x123456,a), byte=value>>24;
        CHECK_EQ(byte,oid|(edge ? 64u : 0u));
        CHECK_EQ(byte<255 ? byte : 255,byte);
    }
    printf("RSX bordes: 131072 píxeles contra r3d_edge_row, vecinos exteriores y 49152 fronteras de profundidad, 0 diferencias\n");
}

static void edge_attribute_passes(void)
{
    static const int delta[]={-513,-1,0,1,513};
    static const unsigned alphas[]={0,15,31};
    unsigned cases=0, different=0;
    for (unsigned bits=0;bits<2048;bits++) for (unsigned ai=0;ai<3;ai++) for (unsigned zi=0;zi<5;zi++) {
        unsigned front=bits&1, equal=(bits>>1)&1, tdepth=(bits>>2)&1, back=(bits>>3)&1;
        unsigned repeat=(bits>>4)&1, trans=(bits>>5)&1, edge=(bits>>6)&1, oldedge=(bits>>7)&1;
        unsigned alpha_ref=(bits&256) ? 15 : 0, w=(bits>>9)&1, blend=(bits>>10)&1, alpha=alphas[ai], depth=10000+delta[zi];
        rsx_pass_input c={0};
        c.id=repeat ? 2 : 3; c.tid=repeat ? 4 : 8; c.oid=1; c.trans_mask=12;
        c.front=front; c.trans=1; c.opaque=1; c.tdepth=tdepth; c.equal=equal;
        c.extended=1; c.blend=blend; c.occupied=1; c.attributes=2; c.prior_back=1;
        sample s={R3D_PX(7,43,59,19),10000,10000,53|(oldedge ? 64u : 0u),32|16|2|(back ? 64u : 0u)|(trans ? 4u : 0u)};
        unsigned metadata=c.id|(edge ? 64u : 0u);
        sample got=execute(s,&c,alpha,alpha_ref,depth,metadata,w,1), want=s;
        r3d_rstate rs={0}; rs.wbuf=w;
        r3d_poly p={0}; p.front=front; p.attr=equal ? PA_DEQUAL : 0;
        unsigned accepted=alpha>alpha_ref && r3d_depth_ok(&p,&rs,depth,s.depth,back ? AT_BACK : 0) &&
            !(alpha<31 && trans && repeat);
        if (accepted) {
            if (alpha==31 || tdepth) want.depth=want.fogdepth=depth;
            if (alpha==31) want.fog=metadata;
        }
        different+=got.depth!=want.depth || got.fogdepth!=want.fogdepth || got.fog!=want.fog;
        cases++;
    }
    CHECK_EQ(different,0);
    printf("RSX bordes: %u planes de atributos, TDEPTH, rechazo TID, opacos y empate frente/espalda Z/W, 0 diferencias\n",cases);
}

static void edge_spans(void)
{
    static gx_frame f;
    unsigned different=0, covered=0, interior=0;
    uint8_t spans[RSX_EDGE_ROW_BYTES];
    for (unsigned shape=0;shape<12;shape++) {
        rsx_edge_test_scene(&f,0);
        gx_poly *gp=&f.poly[0];
        if (shape&1) { f.vtx[0].sx=-23; f.vtx[1].sx=240; f.vtx[2].sx=180; f.vtx[3].sx=4; }
        if (shape&2) { f.vtx[0].sy=-4; f.vtx[1].sy=53; f.vtx[2].sy=191; f.vtx[3].sy=150; }
        if (shape&4) { gp->nverts=3; f.vtx[1].sx=255; f.vtx[2].sy=192; }
        if (shape&8) f.vtx[1].z++;
        const gx_vertex *vv[GX_POLY_MAXV];
        for (unsigned k=0;k<gp->nverts;k++) vv[k]=&f.vtx[gp->vidx[k]];
        r3d_rstate rs; r3d_rstate_init(&rs,&f,0);
        r3d_poly p; CHECK(r3d_poly_setup(&p,gp,vv,&rs));
        rsx_edge_spans(spans,&p);
        CHECK_EQ(spans[RSX_EDGE_FLAT_TEXEL*4+3],!(shape&8));
        for (int y=0;y<R3D_H;y++) {
            r3d_row row; int active=r3d_row_at(&p,y,&row);
            for (int x=0;x<R3D_W;x++) {
                int expected=active && (row.alledge || x<row.xle || x>=row.xrs);
                different+=rsx_edge_span_test(spans+y*4,x)!=expected;
                if (active && x>=row.xl && x<row.xr) { covered++; interior+=!expected; }
            }
        }
    }
    CHECK_EQ(different,0); CHECK(covered>1000 && interior>1000);
    printf("RSX bordes: 12 geometrías, 589824 candidatos de arista, incluidos recorte y pendientes, 0 diferencias\n");
}

static void edge_scenes(void)
{
    static gx_frame f;
    static r3d_ctx ctx;
    static uint8_t vram[R3D_VRAM_ZERO_OFF+128] __attribute__((aligned(128)));
    static uint32_t ref[R3D_W*R3D_H] __attribute__((aligned(128)));
    static uint32_t out[R3D_W*R3D_H] __attribute__((aligned(128)));
    static uint32_t without[R3D_W*R3D_H];
    uint32_t ts[4]={0}, ps[6]={0};
    unsigned different=0, changed=0;
    rsx_edge_test_vram(vram);
    for (unsigned test=0;test<RSX_EDGE_TESTS;test++) {
        rsx_edge_test_scene(&f,test);
        CHECK_EQ(rsx_edge_group(&f,0),rsx_edge_test_group(test));
        CHECK_EQ(rsx_edge_group(&f,R3D_F_NOFOG),rsx_edge_test_group(test)&3u);
        r3d_render_ref(&f,vram,ts,ps,0,ref);
        r3d_render_ref(&f,vram,ts,ps,R3D_F_NOEDGE,without);
        for (unsigned k=0;k<R3D_W*R3D_H;k++) changed+=ref[k]!=without[k];
        if (!test) {
            CHECK_EQ(ref[8*R3D_W+8],R3D_PX(63,0,0,31));
            CHECK_EQ(ref[9*R3D_W+9],R3D_PX(63,63,63,31));
            CHECK_EQ(ref[104*R3D_W+200],R3D_PX(63,63,63,31)); /* ID63 contra fondo ID63 */
        }
        if (test==128) {
            /* El texel visible junto al hueco es interior del polígono:
             * no hereda color de borde por ver fondo en el vecino. */
            CHECK_EQ(ref[96*R3D_W+101],R3D_PX(63,63,63,31));
            CHECK_EQ(ref[96*R3D_W+128],0);
        }
        for (unsigned split=0;split<2;split++) {
            r3d_job j={0};
            j.frame_ea=(uint64_t)(uintptr_t)&f; j.vram_ea=(uint64_t)(uintptr_t)vram;
            j.out_ea=(uint64_t)(uintptr_t)out; j.nunits=split ? 2 : 1; j.band=split ? 8 : 16;
            memset(out,0xa5,sizeof out);
            for (unsigned u=0;u<j.nunits;u++) { r3d_stats st; j.unit=u; CHECK_EQ(r3d_core_render(&ctx,&j,&st),0); }
            if (memcmp(ref,out,sizeof ref)) { different++; printf("RSX bordes escena %u reparto %u difiere\n",test,split); }
        }
    }
    CHECK_EQ(different,0); CHECK(changed>10000);
    printf("RSX bordes: %u escenas Z/W, translúcidos, TDEPTH, sombras, niebla, AA, fondo, huecos y pendientes; referencia/bandas idénticas, %u píxeles marcados\n",RSX_EDGE_TESTS,changed);
}

void test_rsx_passes(void)
{
    edge_pixels();
    edge_attribute_passes();
    edge_spans();
    edge_scenes();
    unsigned compact=sweep(1,12), previous=sweep(0,12), wide=sweep(1,15);
    CHECK(compact>100000 && previous==compact && wide==compact);
    unsigned lotes=batches();
    CHECK(lotes>2000);
    CHECK(shadows_sweep()>100000);
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
    /* TEXUNIT1 del dibujo de color: toon conserva su tabla también con
     * niebla final (la autoprueba de la PS3 vio las densidades como colores
     * toon); la de niebla solo en la ruta por fragmento anterior. */
    for (unsigned mode=0;mode<4;mode++) for (unsigned fog=0;fog<2;fog++) for (unsigned previous=0;previous<2;previous++) {
        unsigned table=rsx_color_table(mode,fog,previous);
        if (mode==2) CHECK_EQ(table,RSX_TABLE_TOON);
        else CHECK_EQ(table,fog && previous ? RSX_TABLE_FOG : RSX_TABLE_NONE);
    }
    printf("RSX pasadas: %u estados contra las reglas escalares por plan (compacto y ronda 6), %u lotes frontales, 0 diferencias\n",compact,lotes);
}
