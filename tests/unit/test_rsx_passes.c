/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "gpu3d/rsx_passes.h"
#include "gpu3d/rsx_capacity_test.h"
#include "gpu3d/rsx_capacity.h"
#include "gpu3d/rsx_modes.h"
#include "gpu3d/r3d_px.h"

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
        if (attributes) {
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
typedef struct dspoly { unsigned shadow, id, alpha, front, tdepth, fog, d; uint32_t rgb; unsigned equal; } dspoly;
static void ds_apply(dspx *x, const dspoly *p, unsigned blend, unsigned alpha_ref, unsigned w)
{
    r3d_rstate rs={0}; rs.wbuf=w;
    r3d_poly poly={0}; poly.front=p->front; poly.attr=p->equal ? PA_DEQUAL : 0;
    int ok=r3d_depth_ok(&poly,&rs,p->d,x->dep,x->back ? AT_BACK : 0);
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
static int shadow_scene_mode(const dspoly *polys, unsigned n, unsigned w, unsigned blend, unsigned alpha_ref,
                            unsigned clear_alpha, unsigned clear_fog, unsigned attributes, int general,
                            unsigned capacity, unsigned *detail, const uint8_t *coverage)
{
    gx_frame *f=&shadow_frame;
    memset(&f->regs,0,sizeof f->regs); f->npolys=n; f->nverts=0; f->swap_params=w ? 3 : 1;
    const unsigned clear_oid=capacity ? 0 : 5;
    f->regs.clear_depth=0x7fff;
    f->regs.clear_color=(clear_oid<<24)|(clear_alpha<<16)|(clear_fog ? 0x8000u : 0u)|0x4210u;
    f->regs.disp3dcnt=(blend ? D3_BLEND : 0)|(alpha_ref ? D3_ALPHATEST : 0);
    f->regs.alpha_test_ref=alpha_ref;
    unsigned shadows=0;
    for (unsigned i=0;i<n;i++) {
        gx_poly *g=&f->poly[i]; memset(g,0,sizeof *g);
        g->attr=(polys[i].id<<24)|(polys[i].alpha<<16)|(polys[i].shadow ? 3u<<4 : 0)|
                (polys[i].tdepth ? PA_TDEPTH : 0)|(polys[i].fog ? PA_FOG : 0)|(polys[i].equal ? PA_DEQUAL : 0);
        g->flags=polys[i].front ? GX_POLY_FRONT : 0; g->nverts=3;
        shadows|=polys[i].shadow;
    }
    r3d_rstate rs; r3d_rstate_init(&rs,f,0);
    rsx_shadow_layout l; rsx_shadow_layout_init(&l,f,&rs,general);
    uint8_t reset[GX_MAX_POLYS]={0};
    if (capacity==2) { if (rsx_capacity_schedule(&l,f,&rs,shadows,NULL,reset)) return -1; }
    else if (capacity) { if (rsx_shadow_layout_capacity(&l,f,&rs,shadows)) return -1; }
    else {
        if (l.translucents>8) return -1;
        if (l.needs_back || l.needs_ids) { if (rsx_shadow_layout_extend(&l,f,&rs,shadows)) return -1; }
    }
    const unsigned ca=clear_alpha!=0;
    const unsigned occupied=(l.ids_mask&16) ? 0 : 16;
    sample s={rs.clear_px,rs.clear_depth,rs.clear_depth>>9,clear_fog,
              l.extended ? ((ca ? occupied : 0u)|l.oid[clear_oid]) : ((ca ? 128u : 0u)|l.oid[clear_oid])};
    dspx x={rs.clear_px,rs.clear_depth,clear_oid,0,0,clear_fog,0,0};
    unsigned prior_back=0;
    for (unsigned i=0;i<n;i++) {
        const dspoly *q=&polys[i];
        if (reset[i]) {
            const rsx_pass clear={3,reset[i],l.trans_mask,reset[i],1,RP_NONE,0,0,RP_MASK,0,0,0};
            const frag unused={0};
            s=execute_frag(s,&clear,attributes,0,&unused);
        }
        if (coverage && !coverage[i]) continue;
        rsx_pass plan[12]; unsigned np;
        frag fr={q->alpha,alpha_ref,q->d,q->fog,w,q->shadow,q->rgb};
        if (q->shadow || !l.extended) {
            rsx_shadow_input in={q->id,l.tid[q->id],l.oid[q->id],l.extended ? l.oid_test[q->id] : 255,
                l.trans_mask,l.opaque_mask,q->front,q->tdepth,q->equal,blend,l.single,
                l.single && q->alpha>alpha_ref,ca,l.extended,l.ids_mask};
            if (!q->shadow) np=rsx_shadow_color_passes(&in,q->alpha<31,plan);
            else np=q->id ? rsx_shadow_passes(&in,plan) : rsx_mask_passes(&in,plan);
        } else {
            rsx_pass_input in={q->id,l.tid[q->id],l.oid[q->id],l.trans_mask,q->front,q->alpha<31,q->alpha==31,
                q->tdepth,q->equal,1,blend,ca,attributes,prior_back,l.ids_mask};
            np=rsx_color_passes(&in,plan);
        }
        prior_back|=!q->front && (q->alpha==31 || q->tdepth);
        for (unsigned k=0;k<np;k++) s=execute_frag(s,&plan[k],attributes,q->tdepth,&fr);
        ds_apply(&x,q,blend,alpha_ref,w);
        if (capacity) {
            unsigned live=x.trans;
            if (capacity==2 && live) {
                live=0;
                for (unsigned future=i+1;future<n;future++)
                    live|=polys[future].id==x.tid && (!polys[future].shadow || x.tid);
            }
            /* Un TID muerto puede conservar su código hasta que se recicle;
             * su equivalencia se comprueba por el resultado de los siguientes
             * polígonos, nunca por el número de un código ya inobservable. */
            unsigned ids=l.oid[x.oid]|(live ? l.tid[x.tid] : 0);
            unsigned persistent=l.ids_mask|64|(shadows ? 32 : 0)|occupied;
            if (capacity==2 && x.trans && !live) persistent&=~l.trans_mask;
            unsigned expected=ids|(x.back ? 64 : 0)|(shadows && x.stencil ? 32 : 0)|
                              (R3D_PX_A(x.col) ? occupied : 0);
            if ((s.stencil&persistent)!=(expected&persistent)) return 0;
        }
    }
    *detail=capacity ? l.ids_mask : l.extended ? 2 : l.single ? 0 : 1;
    /* La pasada de atributos no escribe color: profundidad >>9 y bandera. */
    if (s.depth!=x.dep) return 0;
    if (!attributes && s.color!=x.col) return 0;
    if (attributes && (s.fog!=x.fog || s.fogdepth!=(x.dep>>9))) return 0;
    return 1;
}
static int shadow_scene(const dspoly *polys, unsigned n, unsigned w, unsigned blend, unsigned alpha_ref,
                        unsigned clear_alpha, unsigned clear_fog, unsigned attributes, int general, unsigned *detail)
{
    return shadow_scene_mode(polys,n,w,blend,alpha_ref,clear_alpha,clear_fog,attributes,general,0,detail,NULL);
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
            polys[n++]=(dspoly){0,1,31,!(p0&1),0,(p0>>1)&1,1000,R3D_PX(63,63,63,0),0};
            polys[n++]=(dspoly){1,0,13,!(p1&1),0,0,d1[(p1>>1)&1],0,0};
            polys[n++]=(dspoly){1,(p2&1) ? 1 : 2,alphas[(p2>>1)&1],!((p2>>2)&1),(p2>>3)&1,(p2>>4)&1,d2[(p2>>5)&1],R3D_PX(0,0,0,0),0};
            if (p3==1) polys[n++]=(dspoly){0,2,15,1,(p2>>3)&1,1,998,R3D_PX(0,63,0,0),0};
            if (p3>=2) {
                polys[n++]=(dspoly){1,0,13,1,0,0,999,0,0};
                polys[n++]=(dspoly){1,p3==2 ? 2 : 3,13,1,0,(p2>>4)&1,999,R3D_PX(0,0,63,0),0};
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

static unsigned sweep_mode(int compact, unsigned mask, unsigned ids_mask)
{
    /* mask: bits de TID (12: dos bits con OID; 15: cuatro bits, sin sombras) */
    const unsigned ids=ids_mask ? ids_mask : 15;
    const unsigned occupied=(ids&16) ? 0 : 16, shadow_bit=(ids&32) ? 0 : 32;
    unsigned count=0;
    for (unsigned bit=1;bit<64;bit<<=1) count+=!!(mask&bit);
    const unsigned dst_tid=rsx_id_scatter((1u<<count)-2,mask), other_tid=rsx_id_scatter((1u<<count)-3,mask);
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
        if ((!c.extended && (c.equal || dst_back)) || (empty && dst_trans) || (ids_mask && !c.extended) || (!occupied && empty)) continue;
        c.prior_back=dst_back; c.occupied=!empty;
        c.id=repeated ? 3 : 4; c.tid=repeated ? dst_tid : other_tid; c.oid=mask==ids ? 0 : 1; c.trans_mask=mask;
        c.ids_mask=ids_mask;
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
            s.stencil=c.extended ? ((flags&2) ? 128 : 0)|shadow_bit|(dst_back ? 64 : 0)|(empty ? 0 : occupied)|(dst_trans ? dst_tid : 0)|(mask==ids ? 0 : 2) :
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
                    if (alpha==31) want.stencil=shadow_bit|occupied|c.oid|(c.front ? 0 : 64);
                    else want.stencil=(s.stencil&~(mask|occupied|(c.tdepth ? 64 : 0)))|occupied|c.tid|
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
static unsigned sweep(int compact, unsigned mask) { return sweep_mode(compact,mask,0); }

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

/* Los valores DS no son códigos stencil: comprobar todos los códigos de
 * cada máscara, incluida la discontinuidad del bit de ocupado (máscara 47). */
static void capacity_limits(void)
{
    static const unsigned masks[]={15,31,47,63};
    for (unsigned k=0;k<4;k++) {
        unsigned seen[64]={0}, count=0;
        for (unsigned bit=1;bit<64;bit<<=1) count+=!!(masks[k]&bit);
        for (unsigned value=0;value<(1u<<count);value++) {
            unsigned code=rsx_id_scatter(value,masks[k]);
            CHECK_EQ(code&~masks[k],0); CHECK_EQ(seen[code]++,0);
        }
    }
    static const struct { unsigned shadows, alpha, oid, tid, mask; } limits[]={
        {0,0,0,15,15}, {0,0,0,16,47}, {0,0,0,31,47}, {0,0,0,32,0},
        {0,19,0,16,63}, {0,19,0,31,63}, {0,19,0,63,63}, {0,19,0,64,0},
        {1,0,0,15,15}, {1,0,0,16,0}, {1,19,0,16,31}, {1,19,0,31,31}, {1,19,0,32,0},
        {1,19,1,7,15}, {1,19,1,8,31}, {1,19,1,15,31}, {1,19,1,16,0},
        {1,19,2,3,15}, {1,19,2,4,31}, {1,19,2,7,31}, {1,19,2,8,0},
        {1,19,3,7,31}, {1,19,3,8,0}, {1,0,1,8,0}, {1,0,3,4,0}
    };
    for (unsigned k=0;k<sizeof limits/sizeof limits[0];k++) {
        gx_frame *f=&shadow_frame; memset(f,0,sizeof *f);
        f->regs.clear_color=limits[k].alpha<<16;
        f->regs.clear_depth=0x7fff;
        for (unsigned id=1;id<=limits[k].oid;id++) {
            gx_poly *p=&f->poly[f->npolys++]; p->attr=(id<<24)|(31u<<16); p->flags=GX_POLY_FRONT;
        }
        for (unsigned i=0;i<limits[k].tid;i++) {
            unsigned id=(i+1)&63;
            gx_poly *p=&f->poly[f->npolys++]; p->attr=(id<<24)|(13u<<16)|(limits[k].shadows ? 3u<<4 : 0);
            p->flags=GX_POLY_FRONT;
        }
        r3d_rstate rs; r3d_rstate_init(&rs,f,0);
        rsx_shadow_layout l; rsx_shadow_layout_init(&l,f,&rs,0);
        const char *why=rsx_shadow_layout_capacity(&l,f,&rs,limits[k].shadows);
        if (!!why!=!limits[k].mask || (!why && l.ids_mask!=limits[k].mask)) {
            printf("capacidad: límite %u OID %u TID %u alfa %u sombras %u: máscara %u, motivo %s\n",k,
                   limits[k].oid,limits[k].tid,limits[k].alpha,limits[k].shadows,l.ids_mask,why ? why : "ninguno");
            CHECK(0); continue;
        }
        CHECK(1);
        if (!why) {
            CHECK_EQ(l.opaques,limits[k].oid+1); CHECK_EQ(l.translucents,limits[k].tid+1);
            CHECK_EQ(l.trans_mask&l.opaque_mask,0); CHECK_EQ(l.trans_mask|l.opaque_mask,l.ids_mask);
            unsigned used[64]={0};
            for (unsigned id=0;id<64;id++) if (l.tid[id]!=255) {
                CHECK(l.tid[id]!=0); CHECK_EQ(l.tid[id]&~l.trans_mask,0); CHECK_EQ(used[l.tid[id]]++,0);
            }
        }
    }
    for (unsigned test=0;test<RSX_CAPACITY_TESTS;test++) {
        gx_frame *f=&shadow_frame; rsx_capacity_test_scene(f,test);
        r3d_rstate rs; r3d_rstate_init(&rs,f,0);
        rsx_shadow_layout l; rsx_shadow_layout_init(&l,f,&rs,0);
        const unsigned shadows=rsx_capacity_test_shadows(test);
        const char *old=(shadows && l.translucents>8) ? "capacidad anterior" : rsx_shadow_layout_extend(&l,f,&rs,shadows);
        CHECK(old!=NULL);
        rsx_shadow_layout_init(&l,f,&rs,0);
        const char *why=rsx_shadow_layout_capacity(&l,f,&rs,shadows);
        if (why) {
            uint16_t order[GX_MAX_POLYS]; uint32_t keys[GX_MAX_POLYS]; uint8_t reset[GX_MAX_POLYS];
            for (unsigned i=0;i<f->npolys;i++) {
                unsigned k=i; uint32_t key=r3d_sort_key(&f->poly[i],f->swap_params&1);
                while (k && keys[k-1]>key) { keys[k]=keys[k-1]; order[k]=order[k-1]; k--; }
                keys[k]=key; order[k]=(uint16_t)i;
            }
            why=rsx_capacity_schedule(&l,f,&rs,shadows,order,reset);
        }
        if ((!why)!=rsx_capacity_test_admitted(test)) {
            printf("capacidad: escena PS3 %u, motivo %s\n",test,why ? why : "ninguno"); CHECK(0);
        } else CHECK(1);
        if (!why) CHECK_EQ(l.translucents,rsx_capacity_test_ids(test)+1);
    }
}

static uint32_t capacity_random(uint32_t *state)
{
    *state=*state*1664525u+1013904223u;
    return *state;
}

/* Secuencias largas obligan a escribir los bits recuperados, a rechazar el
 * último TID y a limpiarlo con un opaco. La máscara de sombra, OID, cara,
 * ocupado y puerta se contrastan después de cada polígono, además de color,
 * profundidad y atributos. Los IDs altos no son relleno fuera del píxel. */
static unsigned capacity_sequences(void)
{
    static const struct { unsigned shadows, alpha, oid, tid, mask; } families[]={
        {0,0,0,31,47}, {0,19,0,63,63}, {1,19,0,31,31}, {1,19,1,15,31}, {1,19,3,7,31}
    };
    static const unsigned depths[]={8500,8999,9000,9001,9500,10000,10001};
    unsigned cases=0;
    for (unsigned family=0;family<sizeof families/sizeof families[0];family++)
    for (unsigned flags=0;flags<128;flags++) for (unsigned seed=0;seed<8;seed++) {
        const unsigned shadow=families[family].shadows, oid=families[family].oid, tid=families[family].tid;
        const unsigned w=flags&1, blend=(flags>>1)&1, alpha_ref=(flags&4) ? 13 : 0;
        const unsigned fog=(flags>>3)&1, attributes=(flags>>4)&1, general=(flags>>5)&1;
        dspoly p[320]; unsigned n=0;
        if (shadow) p[n++]=(dspoly){0,0,31,!(flags&64),0,fog,10000,R3D_PX(63,63,63,0),0};
        for (unsigned id=1;id<=oid;id++)
            p[n++]=(dspoly){0,id,31,1,0,fog,10001,R3D_PX(63,63,63,0),0};
        for (unsigned id=1;id<=tid;id++) {
            if (shadow) p[n++]=(dspoly){1,0,13,1,0,fog,20000,0,0};
            p[n++]=(dspoly){shadow,id,19,1,0,id&1,9000,R3D_PX(id&63,(id*7)&63,(id*13)&63,0),0};
        }
        uint32_t random=1u+seed*131u+flags*8191u+family*1048573u;
        for (unsigned i=0;i<128;i++) {
            uint32_t r=capacity_random(&random), r2=capacity_random(&random);
            unsigned type=(r>>20)&3, is_mask=shadow && type==0, is_opaque=type==1;
            unsigned id=is_mask ? 0 : is_opaque ? ((r>>8)%(oid+1)) : 1u+(r%tid);
            unsigned alpha=is_mask ? 13 : is_opaque ? 31 : (r2&7)==0 ? 0 : (r2&7)==1 ? 31 : 13u+(r2%17);
            /* Un alfa31 de color crea un escritor OID adicional: solo los
             * opacos explícitos lo usan en estas familias de sombras. */
            if (!shadow && !is_opaque && alpha==31) alpha=30;
            unsigned is_shadow=shadow && !is_opaque;
            p[n++]=(dspoly){is_shadow,id,alpha,(r2>>8)&1,(r2>>9)&1,(r2>>10)&1,
                            depths[(r2>>12)%7],R3D_PX((r>>2)&63,(r>>8)&63,(r>>14)&63,0),
                            !shadow && ((r2>>15)&1)};
        }
        unsigned detail=0;
        int result=shadow_scene_mode(p,n,w,blend,alpha_ref,families[family].alpha,fog,attributes,general,1,&detail,NULL);
        if (result!=1 || detail!=families[family].mask) {
            printf("capacidad: familia %u flags %u semilla %u: resultado %d máscara %u/%u\n",
                   family,flags,seed,result,detail,families[family].mask);
            CHECK(0); return cases;
        }
        cases++;
    }
    printf("RSX capacidad: %u secuencias largas con 7/15/31/63 TID, máscaras 31/47/63, Z/W y niebla, 0 diferencias\n",cases);
    return cases;
}

static void capacity_intervals(void)
{
    /* Sesenta y cuatro IDs con dos usos contiguos: 31 códigos disponibles
     * con borrado transparente y 63 con borrado ocupado. Los códigos
     * vírgenes se consumen antes de pagar un barrido. Invertir el orden
     * obliga a que reset se indexe por el polígono original. */
    for (unsigned alpha=0;alpha<2;alpha++) for (unsigned reverse=0;reverse<2;reverse++) {
        gx_frame *f=&shadow_frame; memset(f,0,sizeof *f); f->npolys=128;
        f->regs.clear_color=alpha ? 19u<<16 : 0; f->regs.clear_depth=0x7fff;
        uint16_t order[GX_MAX_POLYS]; uint8_t reset[GX_MAX_POLYS];
        for (unsigned i=0;i<128;i++) {
            f->poly[i].attr=((i/2)<<24)|(15u<<16); f->poly[i].flags=GX_POLY_FRONT;
            order[i]=(uint16_t)(reverse ? 127-i : i);
        }
        r3d_rstate rs; r3d_rstate_init(&rs,f,0); rsx_shadow_layout l;
        rsx_shadow_layout_init(&l,f,&rs,0);
        CHECK(rsx_capacity_schedule(&l,f,&rs,0,order,reset)==NULL);
        unsigned nreset=0;
        for (unsigned i=0;i<128;i++) if (reset[i]) {
            nreset++; CHECK_EQ(i&1,reverse); CHECK_EQ(reset[i],l.tid[PA_ID(f->poly[i].attr)]);
        }
        CHECK_EQ(nreset,alpha ? 1 : 33);
        for (unsigned i=0;i<128;i++) f->poly[i].attr=((i%64)<<24)|(15u<<16);
        rsx_shadow_layout_init(&l,f,&rs,0);
        CHECK(rsx_capacity_schedule(&l,f,&rs,0,order,reset)!=NULL);
    }
}

static unsigned capacity_reset_passes(void)
{
    unsigned cases=0;
    for (unsigned mask=1;mask<64;mask++) for (unsigned code=1;code<64;code++) if (!(code&~mask))
    for (unsigned stencil=0;stencil<256;stencil++) {
        const rsx_pass reset={3,code,mask,code,1,RP_NONE,0,0,RP_MASK,0,0,0};
        const frag unused={0};
        sample before={R3D_PX(17,29,41,19),12345,24,1,stencil};
        sample after=execute_frag(before,&reset,0,0,&unused);
        unsigned expected=(stencil&mask)==code ? stencil&~mask : stencil;
        if (after.stencil!=expected || after.color!=before.color || after.depth!=before.depth ||
            after.fogdepth!=before.fogdepth || after.fog!=before.fog) {
            printf("capacidad reset: máscara %u código %u stencil %u: resultado %u/%u\n",mask,code,stencil,after.stencil,expected);
            CHECK(0); return cases;
        }
        cases++;
    }
    printf("RSX capacidad: %u estados de limpieza INVERT conservan OID, máscara, cara, ocupado, Z y color\n",cases);
    return cases;
}

static unsigned capacity_recycled_sequences(void)
{
    unsigned cases=0;
    for (unsigned shadows=0;shadows<2;shadows++) for (unsigned alpha=0;alpha<2;alpha++)
    for (unsigned flags=0;flags<64;flags++) for (unsigned pixel=0;pixel<8;pixel++) {
        dspoly p[1024]; uint8_t coverage[1024]; unsigned n=0;
        const unsigned w=flags&1, blend=(flags>>1)&1, attributes=(flags>>2)&1, fog=(flags>>3)&1;
        const unsigned alpha_ref=(flags&16) ? 13 : 0, back=!!(flags&32);
        const unsigned count=shadows ? 31 : 64;
        /* Dos OID consultables dejan solo tres códigos TID con alfa0.
         * Sus intervalos terminan pronto y ceden códigos a los demás IDs. */
        if (shadows) {
            p[n++]=(dspoly){0,1,31,!back,0,fog,10000,R3D_PX(63,63,63,0),0};
            p[n++]=(dspoly){0,2,31,1,0,fog,10001,R3D_PX(63,63,63,0),0};
        }
        for (unsigned base=0;base<count;base+=2) {
            unsigned ids[4]={base,base+1,base,base+1};
            for (unsigned k=0;k<4;k++) {
                if (ids[k]>=count) continue;
                unsigned id=ids[k]+(shadows ? 1 : 0);
                if (shadows) p[n++]=(dspoly){1,0,15,1,0,fog,20000,0,0};
                p[n++]=(dspoly){shadows,id,19,1,0,k&1,9000,
                               R3D_PX((id*7)&63,(id*13)&63,(id*19)&63,0),0};
                /* Rechazo del mismo ID, también cruzando sombra/color. */
                p[n++]=(dspoly){0,id,15,1,back,1,8500,R3D_PX(63,0,0,0),0};
            }
        }
        for (unsigned i=0;i<n;i++) {
            /* Diferentes píxeles retienen TID muertos. En particular se
             * omite el primer uso de algunos IDs sin omitir su reset. */
            coverage[i]=(uint8_t)(pixel==0 || ((i*13+pixel*7)%11)>pixel);
            if (shadows && i<2) coverage[i]=1;
        }
        unsigned detail=0;
        int result=shadow_scene_mode(p,n,w,blend,alpha_ref,alpha ? 19 : 0,fog,attributes,0,2,&detail,coverage);
        if (result!=1) {
            printf("capacidad temporal: sombras %u alfa %u flags %u píxel %u: resultado %d\n",shadows,alpha,flags,pixel,result);
            CHECK(0); return cases;
        }
        cases++;
    }
    printf("RSX capacidad temporal: %u secuencias con 31/64 TID, códigos reciclados y primeros usos omitidos, 0 diferencias\n",cases);
    return cases;
}

void test_rsx_passes(void)
{
    unsigned compact=sweep(1,12), previous=sweep(0,12), wide=sweep(1,15);
    CHECK(compact>100000 && previous==compact && wide==compact);
    unsigned lotes=batches();
    CHECK(lotes>2000);
    CHECK(shadows_sweep()>100000);
    capacity_limits();
    capacity_intervals();
    CHECK_EQ(capacity_reset_passes(),170240);
    CHECK_EQ(capacity_sequences(),5120);
    CHECK_EQ(capacity_recycled_sequences(),2048);
    unsigned recovered=sweep_mode(1,47,47)+sweep_mode(1,63,63)+sweep_mode(1,28,31);
    CHECK(recovered>1000000);
    printf("RSX capacidad: %u estados de fragmentos mixtos opacos/translúcidos con bits recuperados, 0 diferencias\n",recovered);
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
