/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "gpu3d/rsx_cost.h"
#include "core/nds_priv.h"
#include <stdlib.h>
#include <string.h>

struct native_probe { struct nds *n; int exact, waits; };
static void native_submit(void *ctx, int set, r3d_job *j)
{
    struct native_probe *p=ctx;
    const uint16_t *r=p->n->vid.set[set].frame.eng[0].regs;
    p->exact=rsx_native_exact((uint32_t)r[G2D_R_DISPCAP] | ((uint32_t)r[G2D_R_DISPCAP+1]<<16),0);
    memset((void *)(uintptr_t)j->out_ea,0,R3D_W*R3D_H*4);
}
static void native_wait(void *ctx, int set)
{
    struct native_probe *p=ctx;
    (void)set; p->waits++;
}
static void capture_snapshot(void)
{
    struct nds *n=NULL;
    if (posix_memalign((void **)&n,128,sizeof *n)) abort();
    for (unsigned initial=0;initial<2;initial++) for (unsigned later=0;later<2;later++)
        for (unsigned source=0;source<2;source++) {
            memset(n,0,sizeof *n); nds_init(n);
            struct native_probe probe={n,-1,0};
            n->vid.ops.ctx=&probe; n->vid.ops.submit3d=native_submit; n->vid.ops.wait3d=native_wait;
            nds_vramcnt_write(n,0,0x80);
            nds_bus_write(n,NDS_CPU9,0x04000000,4,0x10000u | G2D_DC_3D | G2D_DC_BG(0));
            uint32_t cap=(initial<<31) | (source<<29) | (1u<<24) | (3u<<20);
            nds_bus_write(n,NDS_CPU9,0x04000064,4,cap);
            nds_video_frame_begin(n);
            CHECK_EQ(probe.exact,initial);
            /* Una escritura durante el cuadro no cambia la instantánea que
             * usa capture_frame, aunque cambie el registro vivo de DISPCAP. */
            nds_bus_write(n,NDS_CPU9,0x04000064,4,(cap&0x7fffffffu) | (later<<31));
            nds_video_frame_end(n);
            CHECK_EQ(probe.waits,initial);
            CHECK_EQ(n->vid.cap.frames,initial);
            CHECK_EQ(n->vid.set[n->vid.cur].captured,initial ? cap : 0);
        }
    free(n);
}

static void samples(rsx_cost *c, unsigned n, int spe, uint64_t us)
{
    for (unsigned i=0;i<n;i++) rsx_cost_sample(c,c->epoch,spe,us);
}
void test_rsx_cost(void)
{
    capture_snapshot();
    rsx_cost c={0};
    CHECK(rsx_cost_spe(&c));
    samples(&c,7,1,10000);
    CHECK_EQ(c.phase,RSX_COST_BASE);
    CHECK_EQ(c.samples,7);
    samples(&c,1,1,10000);
    CHECK_EQ(c.phase,RSX_COST_PROBE);
    CHECK_EQ(c.spe_us,10000);
    /* Dos trabajos en vuelo: el último de la fase anterior no cuenta. */
    CHECK_EQ(rsx_cost_sample(&c,c.epoch-1,1,900000),0);
    CHECK_EQ(rsx_cost_sample(&c,c.epoch,1,900000),0);
    CHECK_EQ(rsx_cost_sample(&c,c.epoch,0,0),0);
    CHECK_EQ(rsx_cost_sample(&c,c.epoch,0,1000001),0);
    CHECK_EQ(c.samples,0);
    samples(&c,8,0,8000); /* frontera de retorno: 8 + 0,5 = 10 * 85 % */
    CHECK_EQ(c.phase,RSX_COST_PROBE);
    samples(&c,8,0,8000);
    CHECK_EQ(c.phase,RSX_COST_RSX);
    samples(&c,8,0,11500); /* empate de salida, conserva HD */
    CHECK_EQ(c.streak,0);
    samples(&c,8,0,11501);
    CHECK_EQ(c.streak,1);
    samples(&c,8,0,11500); /* un pico no cambia ruta */
    CHECK_EQ(c.streak,0);
    samples(&c,16,0,11501);
    CHECK_EQ(c.phase,RSX_COST_SPE);
    samples(&c,119,1,12000);
    CHECK_EQ(c.phase,RSX_COST_SPE);
    samples(&c,1,1,12000);
    CHECK_EQ(c.phase,RSX_COST_PROBE);
    CHECK_EQ(c.spe_us,12000);
    samples(&c,8,0,20000); /* sondeo caro: vuelve a SPE sin segunda ventana */
    CHECK_EQ(c.phase,RSX_COST_SPE);
    samples(&c,120,1,12000);
    samples(&c,16,0,6000);
    CHECK_EQ(c.phase,RSX_COST_RSX);
    samples(&c,120,0,6000);
    CHECK_EQ(c.phase,RSX_COST_BASE); /* renovar referencia SPE */
    samples(&c,8,1,4000); /* tramo nuevo, SPE ahora más barato */
    samples(&c,8,0,6000);
    CHECK_EQ(c.phase,RSX_COST_SPE);

    /* Secuencia larga con pausas, jitter y retiros fuera de generación. */
    unsigned changes=0, previous=c.phase, run=0, slow_spe=0, fast_rsx=0;
    for (unsigned i=0;i<10000;i++) {
        int spe=rsx_cost_spe(&c);
        slow_spe+=i<5000 && spe;
        fast_rsx+=i>=5000 && !spe;
        uint64_t us=spe ? 10000 : i<5000 ? 22000 : 5000;
        rsx_cost_sample(&c,c.epoch,spe,us+(i%3)*50);
        run++;
        if (c.phase!=previous) {
            CHECK(run>=8);
            previous=c.phase; run=0; changes++;
        }
        CHECK_EQ(rsx_cost_sample(&c,c.epoch-1,!spe,1),0);
    }
    CHECK(changes>10 && changes<300);
    CHECK(slow_spe>4500);
    CHECK(fast_rsx>4000);
    for (unsigned scale=1;scale<=4;scale++) for (unsigned verify=0;verify<2;verify++)
        for (unsigned cap=0;cap<2;cap++) for (unsigned old=0;old<2;old++) {
            int exact=rsx_native_exact(cap<<31,verify);
            CHECK_EQ(exact,cap || verify);
            CHECK_EQ(rsx_native_reduce(scale,old,exact),scale>1 && !old && !cap && !verify);
        }
    puts("rsx costo: ventanas, histéresis, sondeos, etiquetas tardías y captura comprobados");
}
