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

static void samples(rsx_cost *c, unsigned n, int spe, uint64_t us, uint64_t frame)
{
    for (unsigned i=0;i<n;i++) {
        uint32_t epoch=c->epoch;
        rsx_cost_sample(c,epoch,spe,us);
        rsx_cost_frame(c,epoch,spe,frame);
    }
}
static void workload(uint64_t rsx, uint64_t spe, uint64_t frame_rsx, uint64_t frame_spe,
                     unsigned *hd, unsigned *native)
{
    rsx_cost c={0};
    *hd=*native=0;
    for (unsigned i=0;i<10000;i++) {
        int native_route=rsx_cost_spe(&c);
        *hd+=!native_route; *native+=native_route;
        samples(&c,1,native_route,native_route ? spe : rsx,native_route ? frame_spe : frame_rsx);
    }
}
void test_rsx_cost(void)
{
    capture_snapshot();
    rsx_cost c={0};
    CHECK(rsx_cost_spe(&c));
    /* Sin medida del cuadro no se decide por los ms crudos del raster. */
    for (unsigned i=0;i<8;i++) rsx_cost_sample(&c,c.epoch,1,650);
    CHECK_EQ(c.phase,RSX_COST_BASE);
    for (unsigned i=0;i<8;i++) rsx_cost_frame(&c,c.epoch,1,9300);
    CHECK_EQ(c.phase,RSX_COST_PROBE);
    CHECK_EQ(c.spe_us,650);
    CHECK_EQ(c.spe_frame_us,9300);
    /* Dos trabajos en vuelo: el último de la fase anterior no cuenta. */
    CHECK_EQ(rsx_cost_sample(&c,c.epoch-1,1,900000),0);
    CHECK_EQ(rsx_cost_sample(&c,c.epoch,1,900000),0);
    CHECK_EQ(rsx_cost_sample(&c,c.epoch,0,0),0);
    CHECK_EQ(rsx_cost_sample(&c,c.epoch,0,1000001),0);
    CHECK_EQ(rsx_cost_frame(&c,c.epoch-1,0,900000),0);
    CHECK_EQ(rsx_cost_frame(&c,c.epoch,1,900000),0);
    CHECK_EQ(rsx_cost_frame(&c,c.epoch,0,0),0);
    CHECK_EQ(rsx_cost_frame(&c,c.epoch,0,1000001),0);
    CHECK_EQ(c.samples,0);
    CHECK_EQ(c.frame_samples,0);
    /* NSMB: RSX hasta cuatro veces más caro, pero sobran ms para HD. */
    samples(&c,8,0,2800,RSX_COST_RETURN);
    CHECK_EQ(c.phase,RSX_COST_PROBE);
    CHECK_EQ(c.reason,RSX_COST_FITS);
    samples(&c,8,0,2800,RSX_COST_RETURN);
    CHECK_EQ(c.phase,RSX_COST_RSX);
    samples(&c,8,0,2800,RSX_COST_BUDGET); /* igualdad: conserva HD */
    CHECK_EQ(c.streak,0);
    CHECK_EQ(c.reason,RSX_COST_MARGIN);
    samples(&c,8,0,8000,18000);
    CHECK_EQ(c.streak,1);
    samples(&c,8,0,2800,14000); /* un pico aislado no cambia ruta */
    CHECK_EQ(c.streak,0);
    samples(&c,16,0,8000,18000);
    CHECK_EQ(c.phase,RSX_COST_SPE);
    CHECK_EQ(c.reason,RSX_COST_OVER);
    samples(&c,119,1,12000,19000);
    CHECK_EQ(c.phase,RSX_COST_SPE);
    samples(&c,1,1,12000,19000);
    CHECK_EQ(c.phase,RSX_COST_PROBE);
    samples(&c,8,0,25000,30000); /* Metroid pesado */
    CHECK_EQ(c.phase,RSX_COST_SPE);
    samples(&c,120,1,650,9300);
    samples(&c,8,0,2800,16001); /* banda entre retorno y salida */
    CHECK_EQ(c.phase,RSX_COST_PROBE);
    CHECK_EQ(c.reason,RSX_COST_MARGIN);
    samples(&c,8,0,2800,14000);
    CHECK_EQ(c.streak,1);
    samples(&c,8,0,2800,16001);
    CHECK_EQ(c.streak,0);
    samples(&c,16,0,2800,14000);
    CHECK_EQ(c.phase,RSX_COST_RSX);
    samples(&c,120,0,2800,14000);
    CHECK_EQ(c.phase,RSX_COST_BASE); /* renovar referencia SPE */
    samples(&c,8,1,650,22000);
    samples(&c,16,0,2800,22000); /* CPU lenta: perder HD no mejora el cuadro */
    CHECK_EQ(c.phase,RSX_COST_RSX);
    CHECK_EQ(c.reason,RSX_COST_NO_GAIN);

    /* Fronteras de mejora: no basta con exceder el presupuesto. */
    c=(rsx_cost){0};
    samples(&c,8,1,10000,15000);
    samples(&c,16,0,11500,18000); /* empate del costo 3D */
    CHECK_EQ(c.phase,RSX_COST_RSX);
    samples(&c,16,0,11501,18000);
    CHECK_EQ(c.phase,RSX_COST_SPE);
    c=(rsx_cost){0};
    samples(&c,8,1,1000,16000);
    samples(&c,16,0,8000,17300); /* 16000 * 1,05 + 500: empate del cuadro */
    CHECK_EQ(c.phase,RSX_COST_RSX);
    samples(&c,16,0,8000,17301);
    CHECK_EQ(c.phase,RSX_COST_SPE);
    c=(rsx_cost){0};
    samples(&c,8,1,6000,14000);
    samples(&c,8,0,22000,12000); /* GPU lenta oculta por flip/solapamiento */
    CHECK_EQ(c.rsx_frame_us,22000);
    CHECK_EQ(c.phase,RSX_COST_SPE);

    unsigned hd,native;
    workload(2800,650,14400,9300,&hd,&native);
    CHECK(hd>9000); /* NSMB barato en ambos, conservar HD */
    workload(25000,12000,30000,19000,&hd,&native);
    CHECK(native>9000); /* Metroid caro: conserva el ahorro aun sin llegar a 60 Hz */
    workload(2800,650,22000,22000,&hd,&native);
    CHECK(hd>9000); /* CPU limitada: ambas rutas son lentas */
    workload(10000,12000,22000,19000,&hd,&native);
    CHECK(hd>9000); /* el raster SPE no mejora, aunque el cuadro difiera */

    /* Jitter tipo NSMB, cambios pesados/ligeros y retiros fuera de generación. */
    c=(rsx_cost){0};
    unsigned changes=0, previous=c.phase, run=0, slow_spe=0, fast_rsx=0;
    for (unsigned i=0;i<10000;i++) {
        int spe=rsx_cost_spe(&c);
        slow_spe+=i<5000 && spe;
        fast_rsx+=i>=5000 && !spe;
        uint64_t us=spe ? (i<5000 ? 12000 : 650) : i<5000 ? 25000 : 1400+(i%5)*350;
        uint64_t frame=spe ? (i<5000 ? 19000 : 9300) : i<5000 ? 30000 : 12400+(i%5)*500;
        samples(&c,1,spe,us,frame);
        run++;
        if (c.phase!=previous) {
            CHECK(run>=8);
            previous=c.phase; run=0; changes++;
        }
        CHECK_EQ(rsx_cost_sample(&c,c.epoch-1,!spe,1),0);
        CHECK_EQ(rsx_cost_frame(&c,c.epoch-1,!spe,1),0);
    }
    CHECK(changes>10 && changes<300);
    CHECK(slow_spe>4500);
    CHECK(fast_rsx>4400);
    /* Retiro realista: dos envíos en vuelo; GPU y cuadro llegan por vías
     * distintas. Ambos órdenes deben progresar y descartar la cola vieja. */
    for (unsigned order=0;order<2;order++) for (unsigned heavy=0;heavy<2;heavy++) {
        struct { uint32_t epoch; int spe; } pending[2]={{0}};
        c=(rsx_cost){0}; hd=native=0;
        for (unsigned i=0;i<10000;i++) {
            uint32_t epoch=c.epoch;
            int spe=rsx_cost_spe(&c);
            hd+=!spe; native+=spe;
            unsigned slot=i&1;
            uint64_t frame=spe ? (heavy ? 19000 : 9300) : heavy ? 30000 : 14400;
            if (order) rsx_cost_frame(&c,epoch,spe,frame);
            if (i>=2) rsx_cost_sample(&c,pending[slot].epoch,pending[slot].spe,
                                     pending[slot].spe ? (heavy ? 12000 : 650) : heavy ? 25000 : 2800);
            if (!order) rsx_cost_frame(&c,epoch,spe,frame);
            pending[slot].epoch=epoch; pending[slot].spe=spe;
        }
        CHECK(heavy ? native>9000 : hd>9000);
    }
    for (unsigned scale=1;scale<=4;scale++) for (unsigned verify=0;verify<2;verify++)
        for (unsigned cap=0;cap<2;cap++) for (unsigned old=0;old<2;old++) {
            int exact=rsx_native_exact(cap<<31,verify);
            CHECK_EQ(exact,cap || verify);
            CHECK_EQ(rsx_native_reduce(scale,old,exact),scale>1 && !old && !cap && !verify);
        }
    puts("rsx costo: presupuesto HD, cuello de botella, histéresis, sondeos, etiquetas tardías y captura comprobados");
}
