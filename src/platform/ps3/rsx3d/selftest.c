/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "gpu3d/rsx_fog_test.h"
#include "gpu3d/rsx_capacity_test.h"
#include <malloc.h>
#include <spukit/spukit.h>
#include "spu_gpu2d_bin.h"
#include "spu/gpu2d/job.h"
#include "gpu2d/gpu2d_scene.h"
#include "gpu2d/gpu2d_hd.h"
#include "gpu2d/gpu2d_compose.h"
#include <sys/stat.h>
#include "platform/ps3/rsx3d/rsx3d.h"
#include "platform/ps3/log.h"
#include "gpu3d/r3d_px.h"
#include "gpu3d/rsx_replay.h"
#include "gpu3d/rsx_vertex.h"
#include "rsxkit/rsxkit.h"
#include "platform/ps3/autotest.h"
#include <sys/systime.h>
static int forced_spe(const char *name)
{
    char path[128]; struct stat st;
    snprintf(path, sizeof path, "/dev_hdd0/salsads/3d-rsx-%s-spe", name);
    return stat(path, &st) == 0;
}
static gx_frame frame;
static uint8_t vram[R3D_VRAM_ZERO_OFF + 128] __attribute__((aligned(128)));
static uint32_t output[R3D_W * R3D_H] __attribute__((aligned(128)));
static uint32_t reference[R3D_W * R3D_H] __attribute__((aligned(128)));
static gx_frame original;
static void save_pixels(const char *name, const uint32_t *px)
{
    char path[160];
    snprintf(path, sizeof path, "/dev_hdd0/salsads/%s.ppm", name);
    FILE *io = fopen(path, "wb");
    if (!io) return;
    fprintf(io, "P6\n256 192\n255\n");
    for (unsigned k = 0; k < R3D_W * R3D_H; k++) {
        fputc(R3D_PX_R(px[k]) * 255 / 63, io);
        fputc(R3D_PX_G(px[k]) * 255 / 63, io);
        fputc(R3D_PX_B(px[k]) * 255 / 63, io);
    }
    fclose(io);
}
static void save_difference(const char *name)
{
    static uint32_t diff[R3D_W*R3D_H];
    for (unsigned k = 0; k < R3D_W*R3D_H; k++) {
        uint32_t a = R3D_PX_A(output[k]), b = R3D_PX_A(reference[k]);
        diff[k] = R3D_PX((!!a != !!b) ? 63 : 0,
                        (a && b && ((output[k]^reference[k]) & 0xffffffu)) ? 63 : 0,
                        a != b ? 63 : 0, 31);
    }
    save_pixels(name, diff);
}
static void preview(void)
{
    static uint32_t pixels[R3D_W * 2 * R3D_H];
    for (unsigned y = 0; y < R3D_H; y++) for (unsigned x = 0; x < R3D_W*2; x++) {
        uint32_t p = (x < R3D_W ? reference : output)[y*R3D_W + x%R3D_W];
        pixels[y*R3D_W*2+x] = (R3D_PX_R(p)*255/63 << 16) | (R3D_PX_G(p)*255/63 << 8) | R3D_PX_B(p)*255/63;
    }
    rsxkit_fb *fb = rsxkit_fb_create(R3D_W*2, R3D_H);
    if (!fb) return;
    rsxkit_view view = {RSXKIT_SCALE_INTEGER, RSXKIT_FILTER_NEAREST, 0};
    rsxkit_fb_upload(fb, pixels, R3D_W*2, R3D_H, R3D_W*8, RSXKIT_XRGB8888);
    rsxkit_frame_begin(0);
    rsxkit_fb_draw(fb, &view, NULL);
    rsxkit_frame_end();
    rsxkit_finish();
    if (!salsads_autotest()) sysUsleep(3000000);
    rsxkit_fb_destroy(fb);
}
static int replay_tests(int enabled)
{
    const char *names[] = { "mkds", "sm64ds", "nsmb", "pesado", "mkds-carrera", "sm64ds-juego", "metroid" };
    int fail = 0;
    for (unsigned scene = 0; scene < sizeof names / sizeof names[0]; scene++) {
        char path[160];
        snprintf(path, sizeof path, "/dev_hdd0/salsads/rsx3d-%s.rsx", names[scene]);
        FILE *io = fopen(path, "rb");
        if (!io) { salsads_log("rsx3d reproducción %s: sin volcado", names[scene]); continue; }
        r3d_job j;
        int valid = r3d_replay_io(io, &frame, &j, vram, 1);
        fclose(io);
        if (!valid) { fail++; continue; }
        j.frame_ea = (uint64_t)(uintptr_t)&frame;
        j.vram_ea = (uint64_t)(uintptr_t)vram;
        j.out_ea = (uint64_t)(uintptr_t)output;
        /* La VRAM grabada permanece inmutable durante sus variantes y repeticiones. */
        static uint64_t serial = 1ull << 63;
        uint64_t versions[41];
        for (unsigned pg = 0; pg < 41; pg++) versions[pg] = serial;
        serial++;
        if (enabled) rsx3d_identity_versions(j.vram_ea, versions);
        rsx3d_stats st;
        memset(&st, 0, sizeof st);
        int native = enabled ? rsx3d_render(&j, &st) : 0;
        salsads_log("rsx3d reproducción %s: cuadro original %u, estado %d, %s, swap %u, disp3d %04x",
                    names[scene], frame.seq, native, rsx3d_reason(), frame.swap_params, frame.regs.disp3dcnt);
        if (native < 0) { fail++; continue; }
        if (scene >= 4 && !native) {
            r3d_render_ref(&frame, vram, j.texslot, j.palslot, j.flags, reference);
            memcpy(output, reference, sizeof output);
            salsads_log("rsx3d reproducción %s: referencia completa; no se filtran efectos", names[scene]);
            preview(); continue;
        }
        /* Diagnóstico explícito del subconjunto de fase 0, nunca usado en juego.
         * Ambas rutas reciben la misma lista filtrada conservando Z o W. */
        unsigned removed = 0;
        if (!native) {
            frame.regs.disp3dcnt &= ~(D3_FOG | D3_EDGE | D3_REARBMP);
            unsigned n = 0;
            for (unsigned i = 0; i < frame.npolys; i++) {
                gx_poly *p = &frame.poly[i];
                if (PA_MODE(p->attr) > 1 || !PA_ALPHA(p->attr) || (p->attr & PA_DEQUAL)) { removed++; continue; }
                frame.poly[n++] = *p;
            }
            frame.npolys = n;
        }
        original = frame;
        for (unsigned variant = 0; variant < (scene >= 3 ? 1u : 4u); variant++) {
            frame = original;
            /* 1/2 aíslan color/coordenadas conservando geometría, alfa y orden.
             * 3 quita texturas y mezcla para observar solo cobertura geométrica. */
            for (unsigned k = 0; k < frame.nverts; k++) {
                if (variant == 1 || variant == 3) frame.vtx[k].r = frame.vtx[k].g = frame.vtx[k].b = 63;
                if (variant == 2) frame.vtx[k].s = frame.vtx[k].t = 0;
            }
            if (variant == 3) {
                frame.regs.disp3dcnt &= ~D3_TEXTURE;
                for (unsigned k = 0; k < frame.npolys; k++) frame.poly[k].attr |= 31u << 16;
            }
            r3d_render_ref(&frame, vram, j.texslot, j.palslot, j.flags, reference);
            rsxkit_frame_begin(0);
            rsxkit_frame_end();
            rsxkit_frame_begin(0);
            if (enabled) rsx3d_identity_versions(j.vram_ea, versions);
            int ok = enabled ? rsx3d_render(&j, &st) : 1;
            if (!enabled) memcpy(output, reference, sizeof output);
            if (ok != 1) { salsads_log("rsx3d reproducción %s: no dibujada, %s", names[scene], rsx3d_reason()); fail++; continue; }
            char name[80];
            snprintf(name, sizeof name, "rsx3d-%s-v%u-rsx", names[scene], variant);
            save_pixels(name, output);
            snprintf(name, sizeof name, "rsx3d-%s-v%u-ref", names[scene], variant);
            save_pixels(name, reference);
            unsigned coverage = 0, color = 0, alpha = 0, maxerr = 0, error1 = 0;
            snprintf(path, sizeof path, "/dev_hdd0/salsads/rsx3d-%s-v%u-diff.ppm", names[scene], variant);
            io = fopen(path, "wb");
            if (io) fprintf(io, "P6\n256 192\n255\n");
            for (unsigned k = 0; k < R3D_W * R3D_H; k++) {
                unsigned a = R3D_PX_A(output[k]), b = R3D_PX_A(reference[k]);
                unsigned cov = !!a != !!b;
                unsigned col = a && b && ((output[k] ^ reference[k]) & 0xffffffu);
                coverage += cov; color += !!col; alpha += a != b;
                unsigned pxerr = 0;
                if (a && b) for (unsigned c = 0; c < 3; c++) {
                    int err = (int)((output[k] >> (8*c)) & 63u) - (int)((reference[k] >> (8*c)) & 63u);
                    unsigned abserr = err < 0 ? -err : err;
                    if (abserr > pxerr) pxerr = abserr;
                    if (abserr > maxerr) maxerr = abserr;
                }
                error1 += col && pxerr <= 1;
                if (io) { fputc(cov ? 255 : 0, io); fputc(col ? 255 : 0, io); fputc(a != b ? 255 : 0, io); }
            }
            if (io) fclose(io); else fail++;
            salsads_log("rsx3d reproducción %s variante %u: %s, omitidos %u, cobertura %u color %u alfa %u error RGB6 máximo %u; polígonos %u dibujos %u texturas %u bytes %u aciertos %u",
                        names[scene], variant, native ? "original" : "subconjunto sin efectos", removed, coverage, color, alpha, maxerr,
                        st.polys, st.draws, st.textures, st.uploaded, st.cache_hits);
            salsads_log("DATO rsx3d reproducción %s v%u: RGB error <=1 %u, raster %.3f ms exportación %.3f ms preparación %.3f ms copia %.3f ms",
                        names[scene], variant, error1, st.raster_ns/1e6, st.export_ns/1e6, st.prepare_us/1e3, st.copy_us/1e3);
            salsads_log("DATO rsx3d HD reproducción %s v%u: escala %ux raster nativo %.3f HD %.3f exportación %.3f PPE %.3f ms",names[scene],variant,rsx3d_scale(),st.raster_ns/1e6,st.hd_raster_ns/1e6,st.export_ns/1e6,st.total_us/1e3);
            salsads_log("DATO rsx3d costo %s v%u: dibujos %u -> %u, estados %u, identidad %llu us vértices %llu us búsqueda %llu us RGBA8 %llu us subida %llu us",
                        names[scene], variant, st.draws_before, st.draws, st.state_changes,
                        (unsigned long long)st.identity_us, (unsigned long long)st.vertex_us,
                        (unsigned long long)st.lookup_us, (unsigned long long)st.decode_us, (unsigned long long)st.upload_us);
            if (!variant) {
                preview();
                if (enabled) {
                    memcpy(reference, output, sizeof output);
                    for (unsigned repeat = 0; repeat < 3; repeat++) {
                        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
                        if (enabled) rsx3d_identity_versions(j.vram_ea, versions);
                        int warm = rsx3d_render(&j, &st);
                        unsigned different = 0;
                        for (unsigned k = 0; k < R3D_W*R3D_H; k++) different += output[k] != reference[k];
                        fail += warm != 1 || different != 0;
                        salsads_log("DATO rsx3d caliente %s %u: estado %d distintos %u dibujos %u estados %u raster %.3f exportación %.3f preparación %.3f ms; identidad %llu vértices %llu búsqueda %llu RGBA8 %llu subida %llu us",
                            names[scene], repeat, warm, different, st.draws, st.state_changes,
                            st.raster_ns/1e6, st.export_ns/1e6, st.prepare_us/1e3,
                            (unsigned long long)st.identity_us, (unsigned long long)st.vertex_us,
                            (unsigned long long)st.lookup_us, (unsigned long long)st.decode_us, (unsigned long long)st.upload_us);
                    }
                    if (rsx3d_scale()>1) fail += rsx3d_hd_bench(j.out_ea,names[scene],&st);
                }
            }
        }
    }
    return fail;
}
static void quad(unsigned n, unsigned id, unsigned alpha, unsigned r, unsigned g, unsigned b, unsigned tex)
{
    gx_poly *p = &frame.poly[n];
    p->attr = (id << 24) | (alpha << 16); p->nverts = 4; p->flags = GX_POLY_FRONT;
    p->ymin = 32; p->ymax = 160; p->xmin = 32; p->xmax = 224;
    p->texparam = tex ? (7u << 26) : 0;
    const int xy[4][2] = {{32,32},{224,32},{224,160},{32,160}};
    for (unsigned k = 0; k < 4; k++) {
        p->vidx[k] = n * 4 + k;
        gx_vertex *v = &frame.vtx[n * 4 + k];
        v->sx = xy[k][0]; v->sy = xy[k][1]; v->z = 0x800000u - n * 0x10000u; v->w = 4096;
        v->r = r; v->g = g; v->b = b; v->s = (k == 1 || k == 2) ? 7 * 16 : 0;
        v->t = k >= 2 ? 7 * 16 : 0;
    }
}
/* Colores constantes: separa aritmética/empacado de interpolación y aristas.
 * Recorre 64 colores de vértice por 32 texeles RGB555, en ambas fórmulas. */
static unsigned shade_tests(r3d_job *j)
{
    unsigned fail = 0;
    for (unsigned mode = 0; mode < 4; mode++) for (unsigned batch = 0; batch < 4; batch++) {
        if (mode > 1 && forced_spe("toon")) continue;
        memset(&frame, 0, sizeof frame);
        frame.npolys = 512; frame.nverts = 2048;
        frame.regs.clear_depth = 0x7fff; frame.regs.disp3dcnt = D3_TEXTURE | D3_BLEND;
        if (mode == 3) frame.regs.disp3dcnt |= D3_HIGHLIGHT;
        for (unsigned t = 0; t < 32; t++)
            frame.regs.toon_table[t] = t | (((t*7)&31) << 5) | ((31-t) << 10);
        for (unsigned t = 0; t < 32; t++) {
            unsigned rgb = t | ((31-t) << 5) | (((t*7)&31) << 10) | 0x8000;
            vram[2*t] = rgb; vram[2*t+1] = rgb >> 8;
        }
        for (unsigned n = 0; n < 512; n++) {
            unsigned c = n & 63, t = batch*8 + n/64;
            unsigned x = (n%32)*8, y = (n/32)*12;
            quad(n, 1, 31, c, 63-c, (c*13)&63, 1);
            gx_poly *p = &frame.poly[n];
            p->attr |= (mode > 1 ? 2u : mode) << 4;
            p->xmin = x; p->xmax = x+8; p->ymin = y; p->ymax = y+12;
            for (unsigned k = 0; k < 4; k++) {
                gx_vertex *v = &frame.vtx[n*4+k];
                v->sx = x + ((k == 1 || k == 2) ? 8 : 0);
                v->sy = y + (k >= 2 ? 12 : 0); v->z = 0x800000;
                v->s = (t%8)*16+8; v->t = (t/8)*16+8;
            }
        }
        r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        rsx3d_stats st;
        int ok = rsx3d_render(j, &st);
        unsigned different = 0;
        for (unsigned k = 0; k < R3D_W*R3D_H; k++) different += output[k] != reference[k];
        fail += ok != 1 || different != 0;
        salsads_log("rsx3d aritmética modo %u lote %u: estado %d, distintos %u", mode, batch, ok, different);
        if (mode > 1) {
            char name[80];
            snprintf(name, sizeof name, "rsx3d-toon-%u-%u-diff", mode, batch); save_difference(name);
        }
    }
    return fail;
}
/* Tabla no monótona, límites de offset/shift, solo alfa y bandera por polígono. */
static void fog_diagnose(const r3d_job *j)
{
    unsigned count[4]={0}, worst[4]={0}, first=R3D_W*R3D_H;
    for (unsigned k=0;k<R3D_W*R3D_H;k++) {
        if (output[k]!=reference[k] && first==R3D_W*R3D_H) first=k;
        for (unsigned c=0;c<4;c++) {
            unsigned a=(output[k]>>(8*c))&255u, b=(reference[k]>>(8*c))&255u, d=a>b ? a-b : b-a;
            count[c]+=d!=0; if (d>worst[c]) worst[c]=d;
        }
    }
    if (first==R3D_W*R3D_H) first=96*R3D_W+128;
    salsads_log("rsx3d niebla canales R/G/B/A: distintos %u/%u/%u/%u máximo %u/%u/%u/%u; píxel %u,%u RSX %08x ref %08x",
                count[0],count[1],count[2],count[3],worst[0],worst[1],worst[2],worst[3],first%R3D_W,first/R3D_W,output[first],reference[first]);
    static uint32_t before[R3D_W*R3D_H];
    uint16_t saved=frame.regs.disp3dcnt; frame.regs.disp3dcnt &= ~D3_FOG;
    r3d_render_ref(&frame,vram,j->texslot,j->palslot,j->flags,before);
    frame.regs.disp3dcnt=saved;
    salsads_log("rsx3d niebla referencia: antes %08x después %08x",before[first],reference[first]);
    rsx3d_fog_diagnose(j,first%R3D_W,first/R3D_W);
    if (first!=96*R3D_W+128) {
        unsigned center=96*R3D_W+128;
        salsads_log("rsx3d niebla centro: antes %08x después %08x RSX %08x",before[center],reference[center],output[center]);
        rsx3d_fog_diagnose(j,128,96);
    }
}
static int render_view_test(r3d_job *j, int reduced, rsx3d_stats *st)
{
    if (!reduced) return rsx3d_render(j,st);
    int ok=rsx3d_submit_view(j,0,0,0,st);
    for (unsigned retry=0; !ok && !strcmp(rsx3d_reason(),"texturas SPE pendientes") && retry<512; retry++) {
        rsx3d_decode_drain(); ok=rsx3d_submit_view(j,0,0,0,st);
    }
    return ok>0 ? rsx3d_wait(0,st) : ok;
}
static int capacity_option(const char *name)
{
    char path[128]; struct stat st;
    snprintf(path,sizeof path,"/dev_hdd0/salsads/3d-rsx-%s",name);
    return stat(path,&st)==0;
}
/* Las escenas nuevas exigen RGB6/A5 exactos; la ampliación no hereda la
 * tolerancia de las pruebas generales de niebla. Con los topes rc1 deben
 * devolver respaldo sin modificar la salida. Restaurar siempre la opción
 * elegida al arrancar; si falla el grupo nuevo, solo se degrada ese grupo. */
static unsigned capacity_tests(r3d_job *j, unsigned *control_fail)
{
    const int previous=rsx3d_capacity_previous();
    unsigned fail=0, rendered=0, fallbacks=0, pixels=0, saved=0;
    unsigned variants=previous ? 1 : rsx3d_scale()>1 ? 3 : 2;
    for (unsigned route=0;route<variants;route++) {
        rsx3d_capacity_set_previous(route==0 ? 1 : previous);
        for (unsigned test=0;test<RSX_CAPACITY_TESTS;test++) {
            const unsigned shadows=rsx_capacity_test_shadows(test);
            const unsigned family=test>>4;
            const unsigned fog=!!(test&2), toon=!!(test&4), back=family==7 || family==12 || !!(test&8), w=!!(test&1);
            rsx_capacity_test_scene(&frame,test);
            int fallback=!route || previous || !rsx_capacity_test_admitted(test) ||
                capacity_option("pasadas-ronda3") ||
                (rsx_capacity_test_ids(test)>7 && (capacity_option("ids-translucidos-8") || forced_spe("sombra-ids"))) ||
                (!shadows && forced_spe("igualdad")) ||
                (shadows && forced_spe("sombra")) || (toon && forced_spe("toon")) ||
                (back && forced_spe("sombra-trasera")) ||
                (back && w && rsx3d_shadow_back_w_spe()) ||
                (fog && (forced_spe("niebla") || !rsx3d_fog_final() ||
                         (shadows && rsx3d_fog_shadow_spe()) || (toon && rsx3d_fog_toon_spe())));
            if (!fallback) r3d_render_ref(&frame,vram,j->texslot,j->palslot,0,reference);
            rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
            memset(output,0xa5,sizeof output);
            rsx3d_stats st; memset(&st,0,sizeof st);
            int ok=render_view_test(j,route==2,&st);
            unsigned different=0;
            for (unsigned k=0;k<R3D_W*R3D_H;k++) different+=output[k]!=(fallback ? 0xa5a5a5a5u : reference[k]);
            unsigned bad=ok!=!fallback || different;
            if (route) fail+=bad; else *control_fail+=bad;
            rendered+=!fallback; fallbacks+=fallback; pixels+=different;
            salsads_log("rsx3d capacidad %s prueba %u: %u IDs %s, %s%s%s, estado %d (%s), distintos exactos %u, dibujos %u, %s",
                        route==0 ? "rc1" : route==1 ? "nativa" : "reducida",test,
                        rsx_capacity_test_ids(test),shadows ? "con sombras" : "translúcidos",
                        w ? "W" : "Z",fog ? "+niebla" : "",toon ? "+toon" : "",ok,
                        fallback ? "respaldo esperado" : "RSX",different,st.draws,rsx3d_reason());
            if (!fallback && bad && saved<8) {
                char name[80]; saved++;
                snprintf(name,sizeof name,"rsx3d-capacidad-%u-%u-rsx",route,test); save_pixels(name,output);
                snprintf(name,sizeof name,"rsx3d-capacidad-%u-%u-ref",route,test); save_pixels(name,reference);
                snprintf(name,sizeof name,"rsx3d-capacidad-%u-%u-diff",route,test); save_difference(name);
                if (fog && ok==1 && route==1) fog_diagnose(j);
            }
        }
    }
    rsx3d_capacity_set_previous(previous);
    salsads_log("rsx3d capacidad: %u escenas RSX, %u respaldos sin modificar salida, %u píxeles distintos exactos, %u fallos nuevos; topes de arranque restaurados %s",
                rendered,fallbacks,pixels,fail,previous ? "rc1" : "ampliados");
    return fail;
}
static unsigned fog_tests(r3d_job *j, int reduced)
{
    if (forced_spe("niebla")) return 0;
    unsigned fail = 0;
    for (unsigned test = 0; test < 18; test++) {
        memset(&frame, 0, sizeof frame);
        frame.npolys = 256; frame.nverts = 1024; frame.swap_params = test & 1 ? 3 : 1;
        frame.regs.clear_depth = 0x7fff;
        frame.regs.clear_color = (31u << 16) | 0x8000u | 0x4210u;
        frame.regs.fog_color = (test % 3 ? 17u : 0u) << 16 | 0x7c1fu;
        /* 16: ambos lados de cada borde Z de la tabla, con offset.
         * 17: densidades enteras y mezcla exacta, incluido 127 -> 128. */
        unsigned shift = test == 16 ? 10 : test == 17 ? 15 : test < 12 ? test : 15;
        frame.regs.disp3dcnt = D3_FOG | (shift << 8) | (test & 2 ? D3_FOG_ALPHA : 0);
        frame.regs.fog_offset = test >= 16 || (test & 4) ? 7000 : 0;
        unsigned step = 1u << (shift > 10 ? 0 : 10-shift);
        for (unsigned t = 0; t < 32; t++) frame.regs.fog_table[t] = t == 31 ? 127 : (t*37)&127;
        for (unsigned n = 0; n < 256; n++) {
            unsigned x = (n%32)*8, y = (n/32)*24;
            quad(n, 1, 31, n&63, 63-(n&63), (n*13)&63, 0);
            gx_poly *p = &frame.poly[n]; p->attr |= n&1 ? PA_FOG : 0;
            if (frame.swap_params & 2) p->wshift = 8;
            p->xmin = x; p->xmax = x+8; p->ymin = y; p->ymax = y+24;
            unsigned z = ((n%34)*step + (n/32)*(step/8) + frame.regs.fog_offset) << 9;
            if (z > 0xfffe00) z = 0xfffe00;
            if (n%7 == 0) z = 512;
            if (test == 16) {
                z = ((7000 + n/8) << 9) + (n%4) - 1;
                p->attr |= PA_FOG;
            }
            if (test == 17) {
                z = (7000 + n%32) << 9;
                frame.regs.fog_table[n%32] = (n%32)*4 + 3;
            }
            for (unsigned k = 0; k < 4; k++) {
                gx_vertex *v = &frame.vtx[n*4+k];
                v->sx = x + ((k == 1 || k == 2) ? 8 : 0);
                v->sy = y + (k >= 2 ? 24 : 0); v->z = z; v->w = z ? z : 1;
            }
        }
        r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        rsx3d_stats st; int ok = render_view_test(j,reduced,&st);
        unsigned different = 0;
        for (unsigned k = 0; k < R3D_W*R3D_H; k++) different += output[k] != reference[k];
        fail += ok != 1 || different != 0;
        salsads_log("rsx3d niebla prueba %u: estado %d, distintos %u, dibujos %u, reducida %u", test, ok, different, st.draws,st.reduced);
        if (!reduced && ok==1 && (different || test==0 || test==5 || test==16)) fog_diagnose(j);
        char name[80];
        snprintf(name, sizeof name, "rsx3d-niebla-%u%s-rsx", test,reduced ? "-reducida" : ""); save_pixels(name, output);
        snprintf(name, sizeof name, "rsx3d-niebla-%u%s-ref", test,reduced ? "-reducida" : ""); save_pixels(name, reference);
        snprintf(name, sizeof name, "rsx3d-niebla-%u%s-diff", test,reduced ? "-reducida" : ""); save_difference(name);
    }
    return fail;
}
static unsigned fog_translucent_tests(r3d_job *j, int reduced)
{
    if (forced_spe("niebla") || !rsx3d_fog_final()) return 0;
    unsigned fail=0, different=0, over=0, worst=0;
    for (unsigned test=0;test<RSX_FOG_TESTS;test++) {
        rsx_fog_test_scene(&frame,test);
        r3d_render_ref(&frame,vram,j->texslot,j->palslot,0,reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        rsx3d_stats st; int ok=render_view_test(j,reduced,&st);
        unsigned bad=0, diff=0;
        for (unsigned k=0;k<R3D_W*R3D_H;k++) {
            uint32_t a=output[k], b=reference[k];
            unsigned d=0;
            for (unsigned c=0;c<3;c++) {
                unsigned x=(a>>(8*c))&255u, y=(b>>(8*c))&255u, dd=x>y ? x-y : y-x;
                if (dd>d) d=dd;
            }
            if (R3D_PX_A(a)!=R3D_PX_A(b)) d=255;
            diff+=a!=b; bad+=d>2; if (d>worst) worst=d;
        }
        fail+=ok!=1 || bad!=0; different+=diff; over+=bad;
        if (ok!=1 || bad) {
            salsads_log("rsx3d niebla final prueba %u: estado %d, distintos %u, fuera de tolerancia %u, reducida %u, %s",test,ok,diff,bad,st.reduced,rsx3d_reason());
            if (fail<=2) {
                char name[80]; snprintf(name,sizeof name,"rsx3d-niebla-final-%u-diff",test); save_difference(name);
                snprintf(name,sizeof name,"rsx3d-niebla-final-%u-rsx",test); save_pixels(name,output);
                snprintf(name,sizeof name,"rsx3d-niebla-final-%u-ref",test); save_pixels(name,reference);
                if (!reduced && ok==1) fog_diagnose(j);
            }
        }
        if (!reduced && ok==1 && !bad && (test==0 || test==16 || test==31)) fog_diagnose(j);
    }
    salsads_log("rsx3d niebla final: %u escenas Z/W, profundidad independiente, AND, solo alfa e ID repetido; distintos %u, fuera de tolerancia %u, peor %u/63; %u fallos; solicitud reducida %u",RSX_FOG_TESTS,different,over,worst,fail,reduced);
    return fail;
}
/* Pendientes y texturas no tienen por qué coincidir con el centro de 1x.
 * El contrato de presentación es una muestra de HD, incluida alfa exacta.
 * Contrastar contra HD exportada completa, con paso uno, detecta desfase,
 * dimensiones equivocadas, filtrado y descriptor de otra ranura. */
static unsigned reduced_sampling_tests(r3d_job *j)
{
    if (rsx3d_scale()==1 || forced_spe("niebla") || !rsx3d_fog_final()) return 0;
    unsigned fail=0, checked=0;
    for (unsigned test=0;test<8;test++) {
        rsx_fog_test_scene(&frame,test*17);
        frame.regs.disp3dcnt|=D3_TEXTURE;
        for (unsigned n=0;n<3;n++) {
            if (test&2) frame.poly[n].texparam=(7u<<26);
            for (unsigned k=0;k<4;k++) {
                gx_vertex *v=&frame.vtx[n*4+k];
                v->sx+=(k==1 ? 7 : k==2 ? -13 : 0);
                v->z+=k*0x20000; v->w+=k*0x10000;
                v->s=(k&1)*127; v->t=(k>>1)*127;
                if (test&4) { v->r=k*19; v->g=63-k*17; }
            }
        }
        for (unsigned i=0;i<64;i++) {
            uint16_t c=0x8000u | ((i*7)&31) | (((i*13)&31)<<5) | (((i*3)&31)<<10);
            if (!(i%5)) c=0;
            vram[2*i]=c; vram[2*i+1]=c>>8;
        }
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        rsx3d_stats st;
        int ok=render_view_test(j,1,&st), bad=ok!=1;
        if (ok==1 && st.reduced) {
            bad=rsx3d_reduce_check(0,output); checked++;
        }
        fail+=bad!=0;
        salsads_log("rsx3d reducción muestra %u: estado %d, reducida %u, diferencias HD %d",test,ok,st.reduced,bad);
    }
    salsads_log("rsx3d reducción: %u imágenes completas contrastadas, %u fallos",checked,fail);
    return fail;
}
/* Máscara por fallo Z, exclusión OID, consumo incluso con fallo y TID repetido.
 * Geometría rectangular constante para aislar el stencil de la interpolación. */
static unsigned shadow_tests(r3d_job *j)
{
    if (forced_spe("sombra")) return 0;
    unsigned fail = 0;
    for (unsigned test = 0; test < 64; test++) {
        if (test == 16 && forced_spe("niebla")) continue;
        memset(&frame, 0, sizeof frame);
        frame.npolys = 5; frame.nverts = 20; frame.swap_params = 1;
        frame.regs.clear_depth = 0x7fff;
        frame.regs.clear_color = (63u << 24) | (31u << 16);
        frame.regs.disp3dcnt = D3_BLEND;
        quad(0, 1, 31, 63, 63, 63, 0);
        quad(1, 0, 13, 0, 0, 0, 0);
        quad(2, test == 1 ? 1 : 2, test == 8 ? 31 : 13, 0, 0, 0, 0);
        quad(3, 0, 13, 0, 0, 0, 0);
        quad(4, test == 3 ? 3 : 2, 13, 0, 0, 0, 0);
        for (unsigned n = 1; n < 5; n++) frame.poly[n].attr |= 3u << 4;
        for (unsigned k = 0; k < 4; k++) {
            frame.vtx[4+k].z = test == 2 ? 0x700000 : 0x900000;
            frame.vtx[12+k].z = test == 4 ? 0x700000 : 0x900000;
            if (test == 5) frame.vtx[8+k].z = 0x900000;
            if (test == 6) frame.vtx[16+k].sx += 32;
        }
        if (test == 7) frame.regs.disp3dcnt = 0;
        if (test == 9) frame.poly[2].attr |= PA_TDEPTH;
        if (test == 10) {
            frame.regs.disp3dcnt |= D3_ALPHATEST;
            frame.regs.alpha_test_ref = 13;
        }
        if (test == 11) {
            frame.poly[0].attr = (1u << 24) | (15u << 16);
            frame.regs.clear_color = 63u << 24;
        }
        if (test == 12 || test == 13) {
            frame.regs.clear_color = 63u << 24;
            frame.regs.clear_depth = 0x4000;
            frame.poly[0].attr = (1u << 24) | (15u << 16);
            for (unsigned k = 0; k < 4; k++) {
                frame.vtx[k].z = 0x700000;
                if (test == 12) frame.vtx[k].sx += 256;
            }
        }
        if (test >= 14 && test < 18) {
            frame.swap_params = 3;
            for (unsigned n = 0; n < 5; n++) for (unsigned k = 0; k < 4; k++)
                frame.vtx[n*4+k].w = n == 0 ? 8192 : n == 1 || n == 3 ? 9000 : 7000;
            if (test == 15) frame.poly[2].attr |= PA_TDEPTH;
        }
        if (test >= 18 && test < 20) {
            frame.npolys = 4; frame.nverts = 16; frame.regs.disp3dcnt = 0;
            frame.regs.clear_depth = 0x4000;
            frame.poly[0].attr = (1u << 24) | (15u << 16);
            for (unsigned k = 0; k < 4; k++) frame.vtx[k].z = 0x700000;
            quad(3, test == 18 ? 2 : 4, 15, 63, 0, 0, 0);
            for (unsigned k = 0; k < 4; k++) frame.vtx[12+k].z = 0x600000;
        }
        if (test >= 20 && test < 24) {
            /* Sombras consecutivas del mismo ID: solape, fallo Z primero,
             * destino vacío y escritura Z; consumir antes de la siguiente. */
            frame.npolys = 4; frame.nverts = 16;
            frame.regs.disp3dcnt = D3_BLEND;
            quad(3, 2, 13, 0, 0, 0, 0);
            frame.poly[3].attr |= 3u << 4;
            if (test == 21) for (unsigned k = 0; k < 4; k++) frame.vtx[8+k].z = 0x900000;
            if (test == 22) {
                frame.regs.clear_color = 63u << 24;
                frame.regs.clear_depth = 0x4000;
                for (unsigned k = 0; k < 4; k++) frame.vtx[k].sx += 256;
            }
            if (test == 23) { frame.poly[2].attr |= PA_TDEPTH; frame.poly[3].attr |= PA_TDEPTH; }
        }
        if (test >= 24 && test < 32) {
            frame.poly[0].flags = 0;
            if (test & 1) frame.poly[2].flags = 0;
            if (test & 2) for (unsigned k=0;k<4;k++) frame.vtx[4+k].z=frame.vtx[k].z;
            if (test & 4) {
                frame.poly[2].attr |= PA_TDEPTH;
                for (unsigned k=0;k<4;k++) frame.vtx[8+k].z=frame.vtx[k].z;
            }
        }
        if (test >= 32 && test < 36) {
            frame.npolys=14; frame.nverts=56;
            for (unsigned n=5;n<14;n++) {
                quad(n, n, (test&1) ? 15 : 31, 63, n*3, 0, 0);
                for (unsigned k=0;k<4;k++) frame.vtx[n*4+k].z=0x700000-n*0x1000;
            }
            if (test&2) frame.poly[0].flags=0;
        }
        if (test >= 36 && test < 44) {
            frame.swap_params=3;
            frame.poly[0].flags=0;
            if (test&1) frame.poly[2].flags=0;
            if (test&4) frame.poly[2].attr|=PA_TDEPTH;
            for (unsigned n=0;n<5;n++) for (unsigned k=0;k<4;k++)
                frame.vtx[n*4+k].w = n==0 ? 8192 : (n==1 || n==3) ? ((test&2) ? 8192 : 9000) : 8192;
        }
        if (test >= 44 && test < 52) {
            frame.regs.clear_color=63u<<24;
            frame.poly[0].flags=0;
            if (test&1) for (unsigned k=0;k<4;k++) frame.vtx[k].sx+=256;
            if (test&2) frame.poly[2].attr|=PA_TDEPTH;
            if (test&4) frame.swap_params=3;
        }
        if (test >= 52 && test < 54) {
            frame.regs.clear_color=63u<<24;
            frame.poly[4].attr=(frame.poly[4].attr&0xffffffu)|(3u<<24);
            frame.npolys=14; frame.nverts=56;
            for (unsigned n=5;n<14;n++) quad(n,n,31,63,n*3,0,0);
            if (test==53) frame.poly[0].flags=0;
        }
        if (test >= 54 && test < 62) {
            /* Una sola máscara: otra pasada no puede ocultar el empate. */
            unsigned bits=test-54;
            frame.npolys=3; frame.nverts=12; frame.swap_params=1;
            frame.regs.clear_color=(63u<<24)|(31u<<16);
            quad(0,1,31,63,63,63,0); quad(1,0,13,0,0,0,0); quad(2,2,13,0,0,0,0);
            frame.poly[0].flags=0;
            frame.poly[1].flags=(bits&4) ? 0 : GX_POLY_FRONT;
            frame.poly[2].flags=(bits&1) ? 0 : GX_POLY_FRONT;
            frame.poly[1].attr|=3u<<4; frame.poly[2].attr|=3u<<4;
            for (unsigned k=0;k<4;k++) {
                frame.vtx[k].z=frame.vtx[8+k].z=0x800000;
                frame.vtx[4+k].z=(bits&2) ? 0x800000 : 0x900000;
            }
        }
        if (test >= 62) {
            frame.swap_params=3; frame.npolys=14; frame.nverts=56;
            frame.regs.clear_color=63u<<24;
            frame.poly[4].attr=(frame.poly[4].attr&0xffffffu)|(3u<<24);
            for (unsigned n=5;n<14;n++) quad(n,n,31,63,n*3,0,0);
            for (unsigned n=0;n<14;n++) for (unsigned k=0;k<4;k++)
                frame.vtx[n*4+k].w=test==62 ? 8192 : n==0 ? 8192 : (n==1 || n==3) ? 9000 : n<5 ? 7000 : 6000-n*32;
        }
        if (test == 16) frame.regs.disp3dcnt |= D3_FOG;
        if (test == 17) {
            frame.regs.disp3dcnt |= D3_EDGE;
            frame.regs.clear_color = (1u << 24) | (31u << 16);
        }
        r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        memset(output,0xa5,sizeof output);
        rsx3d_stats st; int ok = rsx3d_render(j, &st);
        unsigned different = 0;
        int backcase=(test>=24 && test<32) || test==34 || test==35 || (test>=36 && test<52) || test==53 || (test>=54 && test<62);
        /* Cara trasera con W: empates de W entera (wdepth.cgi); respaldo solo
         * con 3d-rsx-sombra-trasera-w-spe o con la W truncada anterior. */
        int fallback = test==33 || test==35 || (backcase && (frame.swap_params&2) && rsx3d_shadow_back_w_spe()) ||
            (backcase && forced_spe("sombra-trasera")) || ((test==52 || test==53 || test>=62) && forced_spe("sombra-ids"));
        for (unsigned k = 0; k < R3D_W*R3D_H; k++) different += output[k] != (fallback ? 0xa5a5a5a5u : reference[k]);
        fail += ok != !fallback || different != 0;
        salsads_log("rsx3d sombra prueba %u: estado %d, distintos %u, centro %08x referencia %08x dibujos %u estados %u raster %.3f ms",
                    test, ok, different, output[96*R3D_W+128], reference[96*R3D_W+128], st.draws, st.state_changes, st.raster_ns/1000000.0);
        char name[80];
        snprintf(name, sizeof name, "rsx3d-sombra-%u-rsx", test); save_pixels(name, output);
        snprintf(name, sizeof name, "rsx3d-sombra-%u-ref", test); save_pixels(name, reference);
        snprintf(name, sizeof name, "rsx3d-sombra-%u-diff", test); save_difference(name);
    }
    return fail;
}
/* Diferencias por píxel con la tolerancia de la niebla final (±2 en RGB6,
 * alfa exacta); devuelve cuántos píxeles distintos y cuántos fuera. */
static void fog_compare(unsigned *different, unsigned *over, unsigned *worst)
{
    *different=*over=*worst=0;
    for (unsigned k=0;k<R3D_W*R3D_H;k++) {
        uint32_t a=output[k], b=reference[k];
        unsigned d=0;
        for (unsigned c=0;c<3;c++) {
            unsigned x=(a>>(8*c))&255u, y=(b>>(8*c))&255u, dd=x>y ? x-y : y-x;
            if (dd>d) d=dd;
        }
        if (R3D_PX_A(a)!=R3D_PX_A(b)) d=255;
        *different+=a!=b; *over+=d>2; if (d>*worst) *worst=d;
    }
}
/* Niebla junto con sombras: la máscara y la sombra en la pasada de atributos
 * (bandera AND, profundidad solo con PA_TDEPTH), Z y W, cara trasera (reparto
 * ampliado, también con W), una o varias sombras, alfa 31, ID repetido. */
static unsigned fog_shadow_tests(r3d_job *j)
{
    if (forced_spe("sombra") || forced_spe("niebla")) return 0;
    unsigned fail=0, total_diff=0, total_over=0;
    for (unsigned test=0;test<48;test++) {
        const unsigned w=test&1, fog_opaque=!!(test&2), fog_shadow=!!(test&4), fog_alpha=!!(test&8), back=!!(test&16);
        const unsigned variant=test>>5; /* 0: base; 1: varias sombras, TDEPTH, borrado con niebla y translúcido del mismo ID */
        memset(&frame,0,sizeof frame);
        frame.npolys=5; frame.nverts=20; frame.swap_params=w ? 3 : 1;
        frame.regs.clear_depth=0x7fff;
        frame.regs.clear_color=(63u<<24)|(31u<<16)|(variant ? 0x8000u : 0u);
        /* Paso de la tabla 1024 (shift 0): densidad parcial e interpolada
         * (Z: opaco 16384>>9 → índice 16, 64/128; W: 20480 → índice 19,
         * 77/128). Con shift 10 la Z caía en la entrada 31 (niebla total,
         * el color de debajo no contaba) y en W la profundidad quedaba por
         * debajo del desplazamiento (sin niebla). */
        frame.regs.disp3dcnt=D3_BLEND|D3_FOG|(fog_alpha ? D3_FOG_ALPHA : 0);
        frame.regs.fog_color=(9u<<16)|0x001fu;
        frame.regs.fog_offset=w ? 1000 : 0;
        for (unsigned t=0;t<32;t++) frame.regs.fog_table[t]=t==31 ? 127 : 4*t+(t&1);
        quad(0,1,31,63,63,63,0);
        quad(1,0,13,0,0,0,0);
        quad(2,2,variant==1 && test&2 ? 31 : 13,0,0,0,0);
        quad(3,0,13,0,0,0,0);
        quad(4,variant ? 3 : 2,13,0,0,0,0);
        for (unsigned n=1;n<5;n++) frame.poly[n].attr|=3u<<4;
        if (!back) frame.poly[0].flags=GX_POLY_FRONT; else frame.poly[0].flags=0;
        if (fog_opaque) frame.poly[0].attr|=PA_FOG;
        if (fog_shadow) { frame.poly[2].attr|=PA_FOG; frame.poly[4].attr|=PA_FOG; }
        if (variant) { frame.poly[2].attr|=PA_TDEPTH; frame.poly[1].attr|=PA_FOG; }
        for (unsigned k=0;k<4;k++) {
            frame.vtx[4+k].z=frame.vtx[12+k].z=0x900000;  /* las máscaras fallan la profundidad */
            frame.vtx[16+k].sx+=32;
        }
        /* W: la profundidad es (w>>wshift)<<wshift; ×1280 mantiene el orden
         * (8192 < 9000, 7000 < 8192) y lleva w>>9 a 16250–22500, dentro de
         * la tabla con el desplazamiento 1000. */
        if (w) for (unsigned n=0;n<5;n++) {
            frame.poly[n].wshift=8;
            for (unsigned k=0;k<4;k++) frame.vtx[n*4+k].w=(n==0 ? 8192 : (n==1 || n==3) ? 9000 : 7000)*1280;
        }
        if (variant) {
            /* Un translúcido normal con el ID de la sombra 2 encima: AND de niebla y rechazo del mismo ID. */
            frame.npolys=6; frame.nverts=24;
            quad(5,2,15,0,63,0,0); frame.poly[5].attr|=PA_FOG;
            for (unsigned k=0;k<4;k++) { frame.vtx[20+k].z=0x7c0000; frame.vtx[20+k].w=6500*1280; }
            if (w) frame.poly[5].wshift=8;
        }
        r3d_render_ref(&frame,vram,j->texslot,j->palslot,0,reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        memset(output,0xa5,sizeof output);
        rsx3d_stats st; int ok=rsx3d_render(j,&st);
        int fallback=rsx3d_fog_shadow_spe() || (back && w && rsx3d_shadow_back_w_spe()) || (back && forced_spe("sombra-trasera"));
        unsigned different=0, over=0, worst=0;
        if (fallback) { for (unsigned k=0;k<R3D_W*R3D_H;k++) different+=output[k]!=0xa5a5a5a5u; over=different; }
        else fog_compare(&different,&over,&worst);
        fail+=ok!=!fallback || over!=0; total_diff+=different; total_over+=over;
        salsads_log("rsx3d niebla+sombra prueba %u: estado %d (%s), distintos %u, fuera de tolerancia %u, peor %u/63, dibujos %u, %s",
                    test,ok,fallback ? "respaldo esperado" : "RSX",different,over,worst,st.draws,rsx3d_reason());
        if (!fallback && (over || test==0 || test==1 || test==21)) {
            char name[80];
            snprintf(name,sizeof name,"rsx3d-niebla-sombra-%u-rsx",test); save_pixels(name,output);
            snprintf(name,sizeof name,"rsx3d-niebla-sombra-%u-ref",test); save_pixels(name,reference);
            snprintf(name,sizeof name,"rsx3d-niebla-sombra-%u-diff",test); save_difference(name);
            if (over && ok==1) fog_diagnose(j);
        }
    }
    salsads_log("rsx3d niebla+sombra: 48 escenas Z/W, cara trasera, una y varias sombras, TDEPTH, alfa 31, ID repetido; distintos %u, fuera de tolerancia %u, %u fallos",total_diff,total_over,fail);
    return fail;
}
/* Niebla junto con toon y resaltado: 512 cuadrados texturados con la tabla
 * de toon, profundidad distinta por cuadrado y bandera alterna; Z y W.
 * Paso de la tabla 1024 (shift 0) y profundidades 4000–32570 (>>9): la
 * densidad es parcial en los dos modos y la Z cabe en 24 bits. (La versión
 * anterior tenía shift 10, con lo que en Z la niebla era total en todos los
 * cuadrados con bandera y en W nula, y en Z desbordaba la profundidad a
 * partir del cuadrado 42 de cada 48.) */
static unsigned fog_toon_tests(r3d_job *j)
{
    if (forced_spe("toon") || forced_spe("niebla")) return 0;
    unsigned fail=0;
    for (unsigned test=0;test<4;test++) {
        const unsigned w=test&1, highlight=test>>1;
        memset(&frame,0,sizeof frame);
        frame.npolys=512; frame.nverts=2048; frame.swap_params=w ? 3 : 1;
        frame.regs.clear_depth=0x7fff; frame.regs.disp3dcnt=D3_TEXTURE|D3_BLEND|D3_FOG|(highlight ? D3_HIGHLIGHT : 0);
        frame.regs.clear_color=(31u<<16)|0x8000u|0x4210u;
        frame.regs.fog_color=(17u<<16)|0x7c1fu;
        frame.regs.fog_offset=4000;
        for (unsigned t=0;t<32;t++) frame.regs.fog_table[t]=t==31 ? 127 : (t*37)&127;
        for (unsigned t=0;t<32;t++) frame.regs.toon_table[t]=t|(((t*7)&31)<<5)|((31-t)<<10);
        for (unsigned t=0;t<32;t++) {
            unsigned rgb=t|((31-t)<<5)|(((t*7)&31)<<10)|0x8000;
            vram[2*t]=rgb; vram[2*t+1]=rgb>>8;
        }
        for (unsigned n=0;n<512;n++) {
            unsigned c=n&63, t=n/64, x=(n%32)*8, y=(n/32)*12;
            quad(n,1,31,c,63-c,(c*13)&63,1);
            gx_poly *p=&frame.poly[n];
            p->attr|=2u<<4; p->attr|=(n&1) ? PA_FOG : 0;
            p->xmin=x; p->xmax=x+8; p->ymin=y; p->ymax=y+12;
            if (w) p->wshift=8;
            /* W: la profundidad es (w>>8)<<8, la misma z>>9 que en Z. */
            uint32_t z=(4000u+(n%48)*600u+(n/48)*37u)<<9;
            for (unsigned k=0;k<4;k++) {
                gx_vertex *v=&frame.vtx[n*4+k];
                v->sx=x+((k==1 || k==2) ? 8 : 0); v->sy=y+(k>=2 ? 12 : 0);
                v->z=z; v->w=z;
                v->s=(t%8)*16+8; v->t=(t/8)*16+8;
            }
        }
        r3d_render_ref(&frame,vram,j->texslot,j->palslot,0,reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        memset(output,0xa5,sizeof output);
        rsx3d_stats st; int ok=rsx3d_render(j,&st);
        int fallback=rsx3d_fog_toon_spe();
        unsigned different=0, over=0, worst=0;
        if (fallback) { for (unsigned k=0;k<R3D_W*R3D_H;k++) different+=output[k]!=0xa5a5a5a5u; over=different; }
        else fog_compare(&different,&over,&worst);
        fail+=ok!=!fallback || over!=0;
        salsads_log("rsx3d niebla+%s %s: estado %d (%s), distintos %u, fuera de tolerancia %u, peor %u/63, dibujos %u, %s",
                    highlight ? "resaltado" : "toon",w ? "W" : "Z",ok,fallback ? "respaldo esperado" : "RSX",different,over,worst,st.draws,rsx3d_reason());
        if (!fallback) {
            char name[80];
            snprintf(name,sizeof name,"rsx3d-niebla-toon-%u-rsx",test); save_pixels(name,output);
            snprintf(name,sizeof name,"rsx3d-niebla-toon-%u-ref",test); save_pixels(name,reference);
            snprintf(name,sizeof name,"rsx3d-niebla-toon-%u-diff",test); save_difference(name);
            /* Dato crudo de los primeros cuadrados que fallan (centro de
             * cada uno): color y alfa RSX y referencia, Z>>9, bandera de
             * niebla, índice de toon y texel. */
            unsigned logged=0;
            for (unsigned n=0;n<512 && logged<4;n++) {
                unsigned x=(n%32)*8+4, y=(n/32)*12+6, k=y*R3D_W+x;
                uint32_t a=output[k], b=reference[k], d=0;
                for (unsigned ch=0;ch<3;ch++) {
                    unsigned p=(a>>(8*ch))&255u, q=(b>>(8*ch))&255u, dd=p>q ? p-q : q-p;
                    if (dd>d) d=dd;
                }
                if (d<=2 && R3D_PX_A(a)==R3D_PX_A(b)) continue;
                salsads_log("rsx3d niebla+%s %s cuadrado %u (%u,%u): RSX %u,%u,%u a%u ref %u,%u,%u a%u; z>>9 %u bandera %u toon %u texel %u",
                            highlight ? "resaltado" : "toon",w ? "W" : "Z",n,x,y,
                            R3D_PX_R(a),R3D_PX_G(a),R3D_PX_B(a),R3D_PX_A(a),R3D_PX_R(b),R3D_PX_G(b),R3D_PX_B(b),R3D_PX_A(b),
                            (unsigned)(frame.vtx[n*4].z>>9),!!(frame.poly[n].attr&PA_FOG),(n&63)>>1,n/64);
                logged++;
            }
            if (over && ok==1) fog_diagnose(j);
        }
    }
    return fail;
}
static unsigned equality_tests(r3d_job *j)
{
    const int forced=forced_spe("igualdad");
    unsigned fail=0;
    const int delta[]={-513,-512,-511,-1,0,1,511,512,513};
    for (unsigned t=0;t<81;t++) {
        unsigned c=t%36, w=c/18, n=c%9;
        memset(&frame,0,sizeof frame);
        frame.npolys=2; frame.nverts=8; frame.swap_params=w ? 3 : 1;
        frame.regs.clear_depth=0x7fff; frame.regs.clear_color=31u<<16;
        frame.regs.disp3dcnt=D3_BLEND;
        quad(0,1,31,63,0,0,0); quad(1,2,c%18>=9 ? 15 : 31,0,63,0,0);
        frame.poly[1].attr|=PA_DEQUAL;
        int d=delta[n];
        if (w && (d>1 || d< -1)) d=d>0 ? d-257 : d+257;
        for (unsigned k=0;k<4;k++) {
            frame.vtx[k].z=0x700000; frame.vtx[4+k].z=0x700000+d;
            frame.vtx[k].w=8192; frame.vtx[4+k].w=8192+d;
        }
        if (t>=36 && t<72) {
            frame.regs.clear_color=0;
            if (!w) for (unsigned k=0;k<4;k++) {
                unsigned slope=(k==1 || k==2) ? 0x60000 : 0;
                frame.vtx[k].z+=slope; frame.vtx[4+k].z+=slope;
            }
        }
        if (t>=72) {
            frame.regs.disp3dcnt|=D3_FOG;
            frame.regs.fog_color=(31u<<16)|31u;
            for (unsigned i=0;i<32;i++) frame.regs.fog_table[i]=64;
            frame.poly[1].attr|=PA_FOG;
        }
        r3d_render_ref(&frame,vram,j->texslot,j->palslot,0,reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        memset(output,0xa5,sizeof output);
        rsx3d_stats st; int ok=rsx3d_render(j,&st); unsigned different=0;
        for (unsigned k=0;k<R3D_W*R3D_H;k++) different+=output[k]!=(forced ? 0xa5a5a5a5u : reference[k]);
        fail+=ok!=!forced || different!=0;
        salsads_log("rsx3d igualdad prueba %u: estado %d distintos %u dibujos %u raster %.3f ms",t,ok,different,st.draws,st.raster_ns/1000000.0);
    }
    return fail;
}
static unsigned fallback_tests(r3d_job *j)
{
    unsigned fail = 0;
    for (unsigned test = 0; test < 10; test++) {
        memset(&frame, 0, sizeof frame);
        frame.npolys = 2; frame.nverts = 8; frame.regs.clear_depth = 0x7fff;
        quad(0, 1, 31, 63, 0, 0, 0); quad(1, 2, 13, 0, 0, 0, 0);
        frame.poly[1].attr |= 3u << 4;
        if (test == 0) frame.regs.disp3dcnt = D3_REARBMP;
        if (test == 1) frame.regs.disp3dcnt = D3_EDGE;
        if (test == 2) frame.poly[0].attr |= PA_DEQUAL;
        if (test == 3) frame.poly[0].attr &= ~(31u << 16);
        if (test == 4) {
            frame.npolys=18; frame.nverts=72;
            for (unsigned n=2;n<18;n++) quad(n,n+2,15,63,0,0,0);
        }
        if (test == 5) { frame.regs.disp3dcnt = D3_FOG; frame.regs.fog_table[0] = 1; }
        if (test >= 6 && test < 8) {
            frame.npolys = 1; frame.nverts = 4;
            frame.regs.disp3dcnt = D3_FOG; frame.regs.fog_table[0] = 1;
            frame.poly[0].attr |= PA_FOG;
            frame.swap_params = test == 7 ? 3 : 1;
            frame.vtx[1].z++; frame.vtx[1].w++;
        }
        if (test==8 || test==9) {
            frame.regs.disp3dcnt=D3_BLEND; frame.regs.clear_color=31u<<16;
            quad(1,2,31,0,63,0,0); frame.poly[1].attr|=PA_DEQUAL;
            if (test==8) { frame.swap_params=3; frame.poly[0].flags=0; }
            else {
                frame.npolys=18; frame.nverts=72;
                for (unsigned n=2;n<18;n++) quad(n,n+2,15,63,0,0,0);
            }
        }
        memset(output, 0xa5, sizeof output);
        /* Como en las pruebas de niebla: RPCS3 solo renueva una textura de memoria principal reescrita por el PPE
         * (la tabla de 128 bytes) tras un flip; sin él leía la tabla de la última prueba de niebla. */
        if (test >= 6 && test < 8) { rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0); }
        rsx3d_stats st; int ok = rsx3d_render(j, &st);
        unsigned modified = 0;
        for (unsigned k = 0; k < R3D_W*R3D_H; k++) modified += output[k] != 0xa5a5a5a5u;
        /* 5 (niebla con sombra) y 8 (igualdad con cara trasera en W) ya se
         * admiten: exigir la imagen exacta de la referencia. */
        if ((test == 5 && !rsx3d_fog_shadow_spe()) || (test == 8 && !rsx3d_shadow_back_w_spe())) {
            r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
            unsigned differ = 0;
            for (unsigned k = 0; k < R3D_W*R3D_H; k++) differ += output[k] != reference[k];
            fail += ok != 1 || differ != 0;
            salsads_log("rsx3d respaldo prueba %u: estado %d (admitida), distintos %u, %s", test, ok, differ, rsx3d_reason());
            continue;
        }
        if (test >= 6 && test < 8 && !rsx3d_fog_constant()) {
            /* Niebla por fragmento: el RSX admite el cuadro; se exige la tolerancia documentada
             * (±1 LSB de 5 bits por canal = ±2 en RGB6, alfa exacta) contra la referencia. */
            r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
            unsigned over = 0, differ = 0, worst = 0, first = R3D_W*R3D_H;
            for (unsigned k = 0; k < R3D_W*R3D_H; k++) {
                uint32_t a = output[k], b = reference[k];
                unsigned d = 0;
                if (a != b && first == R3D_W*R3D_H) first = k;
                for (unsigned c = 0; c < 3; c++) {
                    unsigned x = (a >> (8*c)) & 255u, y = (b >> (8*c)) & 255u, dd = x > y ? x - y : y - x;
                    if (dd > d) d = dd;
                }
                if (R3D_PX_A(a) != R3D_PX_A(b)) d = 255;
                differ += d != 0; over += d > 2; if (d > worst) worst = d;
            }
            fail += ok != 1 || over != 0;
            salsads_log("rsx3d respaldo prueba %u: estado %d (niebla %s), distintos %u, fuera de tolerancia %u, peor %u/63, %s", test, ok,
                        rsx3d_fog_final() ? "final" : "por fragmento", differ, over, worst, rsx3d_reason());
            if (first < R3D_W*R3D_H)
                salsads_log("rsx3d respaldo prueba %u: primer distinto en %u,%u: RSX %08x referencia %08x; z0 %u z1 %u w0 %u w1 %u", test, first % R3D_W, first / R3D_W,
                            output[first], reference[first], frame.vtx[0].z, frame.vtx[1].z, (unsigned)frame.vtx[0].w, (unsigned)frame.vtx[1].w);
            if (ok==1 && over) fog_diagnose(j);
            continue;
        }
        fail += ok != 0 || modified != 0;
        salsads_log("rsx3d respaldo prueba %u: estado %d, salida modificada %u, %s", test, ok, modified, rsx3d_reason());
    }
    return fail;
}
/* La W correcta es armónica en pantalla, no la Z lineal de los vértices.
 * En el centro gana verde (W=5461); con interpolación lineal ganaría rojo. */
static unsigned w_tests(r3d_job *j)
{
    unsigned fail = 0;
    for (unsigned shift = 0; shift <= 8; shift += 4) {
        memset(&frame, 0, sizeof frame);
        frame.npolys = 2; frame.nverts = 8; frame.swap_params = 3;
        frame.regs.clear_depth = 0x7fff; frame.regs.disp3dcnt = D3_BLEND;
        quad(0, 1, 31, 0, 63, 0, 0);
        quad(1, 2, 31, 63, 0, 0, 0);
        for (unsigned k = 0; k < 4; k++) {
            frame.vtx[k].w = (k == 1 || k == 2) ? 8192 : 4096;
            frame.vtx[4+k].w = 6000; frame.vtx[4+k].z = 0x900000;
        }
        frame.poly[0].wshift = frame.poly[1].wshift = shift;
        r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        rsx3d_stats st; int ok = rsx3d_render(j, &st);
        unsigned different = 0;
        for (unsigned x = 80; x <= 176; x += 48) {
            unsigned k = 96*R3D_W+x;
            different += output[k] != reference[k];
            salsads_log("rsx3d W shift %u x %u: %08x referencia %08x", shift, x, output[k], reference[k]);
        }
        fail += ok != 1 || different != 0;
    }
    return fail;
}
/* Sonda de cuantización de la mezcla fija RGBA8: no declara paridad. */
static void blend_probe(r3d_job *j)
{
    memset(&frame, 0, sizeof frame);
    frame.npolys = 30; frame.nverts = 120; frame.swap_params = 1;
    frame.regs.clear_depth = 0x7fff; frame.regs.clear_color = (31u << 16) | 0x7fff;
    frame.regs.disp3dcnt = D3_BLEND;
    for (unsigned n = 0; n < 30; n++) {
        quad(n, 1, n+1, 63, 63, 63, 0);
        gx_poly *p = &frame.poly[n]; p->xmin = n*8; p->xmax = n*8+8;
        for (unsigned k = 0; k < 4; k++) {
            frame.vtx[n*4+k].sx = n*8 + ((k == 1 || k == 2) ? 8 : 0);
        }
    }
    r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
    rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
    rsx3d_stats st; int ok = rsx3d_render(j, &st);
    for (unsigned n = 0; n < 30; n++) {
        unsigned k = 96*R3D_W + n*8+4;
        salsads_log("rsx3d límite mezcla RGBA8 alfa %u: estado %d, %08x referencia %08x", n+1, ok, output[k], reference[k]);
    }
}
/* Identidad exacta: cambios ajenos, texeles, paleta y reasignación de bancos. */
static unsigned cache_tests(r3d_job *j)
{
    unsigned fail = 0;
    memset(&frame, 0, sizeof frame);
    frame.npolys = 1; frame.nverts = 4; frame.regs.clear_depth = 0x7fff;
    frame.regs.disp3dcnt = D3_TEXTURE | D3_BLEND;
    quad(0, 1, 31, 63, 63, 63, 1);
    for (unsigned i = 0; i < 64; i++) { vram[2*i] = 0xe0; vram[2*i+1] = 0x83; }
    for (unsigned step = 0; step < 6; step++) {
        if (step == 1) vram[0x80000] ^= 1;
        if (step == 2) for (unsigned i = 0; i < 64; i++) { vram[2*i] = 0x1f; vram[2*i+1] = 0x80; }
        if (step == 3) {
            frame.poly[0].texparam = 2u << 26;
            memset(vram, 0x55, 16); j->palslot[0] = 0x20000;
            vram[0x20002] = 0xe0; vram[0x20003] = 3;
        }
        if (step == 4) { vram[0x20002] = 31; vram[0x20003] = 0; }
        if (step == 5) { j->texslot[0] = 0x40000; memset(vram + 0x40000, 0, 16); }
        r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
        rsxkit_frame_begin(0); rsxkit_frame_end(); rsxkit_frame_begin(0);
        rsx3d_stats st; int ok = rsx3d_render(j, &st);
        unsigned different = 0;
        for (unsigned k = 0; k < R3D_W*R3D_H; k++) different += output[k] != reference[k];
        fail += ok != 1 || different != 0;
        salsads_log("rsx3d identidad paso %u: estado %d distintos %u decodificadas %u", step, ok, different, st.textures);
    }
    return fail;
}
/* Una textura mayor que el presupuesto no publica filas incompletas. Una
 * escritura entre lotes obliga a rehacer lo ya decodificado de esa página. */
static unsigned cold_tests(r3d_job *j)
{
    struct stat flag;
    if (stat("/dev_hdd0/salsads/3d-rsx-texturas-ppe", &flag) == 0) return 0;
    unsigned fail = 0;
    memset(&frame, 0, sizeof frame); memset(vram, 0, sizeof vram);
    frame.npolys = 1; frame.nverts = 4; frame.regs.clear_depth = 0x7fff;
    frame.regs.disp3dcnt = D3_TEXTURE;
    quad(0, 1, 31, 63, 63, 63, 1);
    frame.poly[0].texparam = (7u << 26) | (6u << 20) | (6u << 23);
    for (unsigned i = 0; i < 4; i++) {
        j->texslot[i] = 0;
        frame.vtx[i].s = (i == 1 || i == 2) ? 511*16 : 0;
        frame.vtx[i].t = i >= 2 ? 511*16 : 0;
    }
    /* Color uniforme: esta prueba mide vida útil, no precisión del muestreo. */
    for (unsigned i = 0; i < 0x20000; i += 2) { vram[i] = 31; vram[i+1] = 0x80; }
    memset(output, 0xcd, sizeof output);
    rsx3d_stats st;
    int first = rsx3d_submit(j, 0, &st);
    fail += first != 0;
    fail += output[0] != 0xcdcdcdcdu;
    rsx3d_decode_drain();
    for (unsigned i = 0; i < 0x20000; i += 2) { vram[i] = 0xe0; vram[i+1] = 0x83; }
    int changed = rsx3d_submit(j, 0, &st);
    fail += changed != 0;
    fail += output[0] != 0xcdcdcdcdu;
    rsx3d_decode_drain();
    int ready = rsx3d_render(j, &st);
    fail += ready != 1;
    r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
    unsigned different = 0;
    for (unsigned i = 0; i < R3D_W*R3D_H; i++) different += output[i] != reference[i];
    fail += different != 0;
    salsads_log("rsx3d frío: estados %d/%d/%d, distintos %u, publicación aplazada e invalidación entre lotes, %u fallos",
                first, changed, ready, different, fail);
    if (fail) { save_pixels("rsx3d-frio-rsx", output); save_pixels("rsx3d-frio-ref", reference); }
    return fail;
}
/* Muestreo de un texel aislado: mapa diagnóstico A/B, separado de la
 * prueba de publicación. La precisión global RSX/DS no está certificada. */
static unsigned cold_sampling_probe(r3d_job *j)
{
    memset(&frame, 0, sizeof frame); memset(vram, 0, sizeof vram);
    frame.npolys = 1; frame.nverts = 4; frame.regs.clear_depth = 0x7fff;
    frame.regs.disp3dcnt = D3_TEXTURE;
    quad(0, 1, 31, 63, 63, 63, 1);
    frame.poly[0].texparam = (7u << 26) | (6u << 20) | (6u << 23);
    for (unsigned i = 0; i < 4; i++) { j->texslot[i] = 0; frame.vtx[i].s = frame.vtx[i].t = 8; }
    vram[0] = 0xe0; vram[1] = 0x83;
    rsx3d_stats st; int result = rsx3d_render(j, &st);
    r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, reference);
    unsigned different = 0;
    for (unsigned i = 0; i < R3D_W*R3D_H; i++) different += output[i] != reference[i];
    save_pixels("rsx3d-frio-muestreo", output);
    salsads_log("rsx3d diagnóstico texel aislado 512: estado %d, distintos de DS %u; comparar mapa con rutas anteriores", result, different);
    return result != 1;
}
/* Dos envíos antes de retirar: cambiar lista, textura y destino no puede
 * alterar el cuadro anterior. Retirar también en orden inverso prueba etiquetas. */
static unsigned overlap_tests(r3d_job *j, int diagnostic, int distinct, int reduced)
{
    static uint32_t want[2][R3D_W * R3D_H], got[2][R3D_W * R3D_H];
    static uint32_t previous[R3D_W * R3D_H];
    unsigned native[3]={0}, hd[4]={0};
    unsigned fail = 0;
    uint64_t saved_out = j->out_ea;
    if (diagnostic) salsads_log("rsx3d HD diagnóstico inicio: descriptores %s; lectura %s",
        distinct ? "distintos" : "originales", diagnostic==2 ? "composición entre parejas (altera sincronía)" : "nativa, secuencia original");
    for (unsigned round = 0; round < 8; round++) {
        int posted[2] = {0, 0};
        for (unsigned slot = 0; slot < 2; slot++) {
            memset(&frame, 0, sizeof frame);
            frame.npolys = 1; frame.nverts = 4; frame.swap_params = round & 1 ? 3 : 1;
            frame.regs.clear_depth = 0x7fff; frame.regs.disp3dcnt = D3_TEXTURE;
            quad(0, 1, 31, 63, 63, 63, 1);
            uint16_t color = (uint16_t)(0x8000u | ((round * 7 + slot * 13) & 31u) |
                                        (((round + slot * 9) & 31u) << 5));
            for (unsigned i = 0; i < 64; i++) { vram[2*i] = color; vram[2*i+1] = color >> 8; }
            j->texslot[0] = 0;
            r3d_render_ref(&frame, vram, j->texslot, j->palslot, 0, want[slot]);
            memset(got[slot], 0xcd, sizeof got[slot]);
            j->out_ea = (uint64_t)(uintptr_t)got[slot];
            rsx3d_stats st;
            int result = rsx3d_submit_view(j, slot, !reduced, 0, &st);
            for (unsigned retry = 0; !result && !strcmp(rsx3d_reason(), "texturas SPE pendientes") && retry < 512; retry++) {
                fail += got[slot][0] != 0xcdcdcdcdu;
                rsx3d_decode_drain(); result = rsx3d_submit_view(j, slot, !reduced, 0, &st);
            }
            posted[slot] = result == 1;
            fail += !posted[slot];
            if (posted[slot]) fail += rsx3d_submit(j, slot, &st) != -1;
            /* El destino del consumidor solo se publica al retirar. */
            fail += got[slot][0] != 0xcdcdcdcdu;
        }
        memset(&frame, 0, sizeof frame); memset(vram, 0, 128);
        for (unsigned k = 0; k < 2; k++) {
            unsigned slot = k ^ (round & 1u);
            rsx3d_stats st;
            if (posted[slot]) {
                fail += rsx3d_wait(slot, &st) != 1;
                unsigned different=0,first=0;
                for (unsigned px=0;px<R3D_W*R3D_H;px++) if (got[slot][px]!=want[slot][px]) { if (!different) first=px; different++; }
                if (different) salsads_log("rsx3d solapamiento detalle: vuelta %u ranura %u distintos %u primero %u %08x != %08x",round,slot,different,first,got[slot][first],want[slot][first]);
                if (diagnostic) {
                    unsigned f=round*2+slot, prev=0;
                    for (unsigned px=0;px<R3D_W*R3D_H;px++)
                        prev += !f || got[slot][px]!=(slot ? want[0] : previous)[px];
                    unsigned kind=!different ? 0 : !prev ? 1 : 2;
                    native[kind]++;
                    salsads_log("rsx3d HD diagnóstico: descriptores %s cuadro %u ranura %u nativo %s; diferencias actual %u anterior %u",
                        distinct ? "distintos" : "originales",f,slot,
                        kind==0 ? "exacto" : kind==1 ? "igual al cuadro anterior" : "otro",different,prev);
                } else fail += different != 0;
                fail += rsx3d_wait(slot, &st) != -1;
            }
        }
        if (diagnostic==2) for (unsigned slot=0;slot<2;slot++) {
            unsigned f=round*2+slot;
            int result=posted[slot] ? rsx3d_hd_sequence((uint64_t)(uintptr_t)got[slot],slot,f,want[slot],f ? (slot ? want[0] : previous) : NULL) : -1;
            hd[result<0 ? 3 : result]++;
        }
        if (diagnostic) memcpy(previous,want[1],sizeof previous);
    }
    if (diagnostic) salsads_log("rsx3d HD diagnóstico resumen: descriptores %s lectura %s; nativo exacto %u anterior %u otro %u; protocolo %u",
        distinct ? "distintos" : "originales",diagnostic==2 ? "entre parejas" : "original",native[0],native[1],native[2],fail);
    if (diagnostic==2) salsads_log("rsx3d HD composición resumen: descriptores %s; exacto %u anterior %u otro %u error %u; lectura entre parejas",
        distinct ? "distintos" : "originales",hd[0],hd[1],hd[2],hd[3]);
    j->out_ea = saved_out;
    salsads_log("rsx3d solapamiento: 16 cuadros, %u fallos de %s, solicitud reducida %u", fail,diagnostic ? "protocolo" : "paridad",reduced);
    return fail;
}

/* El mismo banco permite decidir escalar/VMX en la consola durante la
 * autoprueba, sin atribuir a la consola los tiempos del reproductor. */
static void vertex_bench(void)
{
    gx_vertex in[256] __attribute__((aligned(16)));
    rsx3d_vertex out[256] __attribute__((aligned(16)));
    uint64_t elapsed[2] = {0, 0}; uint32_t seed = 19;
    for (unsigned i = 0; i < 256; i++) {
        seed = seed*1664525u+1013904223u;
        memset(&in[i], 0, sizeof in[i]);
        in[i].sx = seed; in[i].sy = seed >> 16; in[i].z = seed & 0xffffff;
        in[i].s = seed >> 3; in[i].t = seed >> 5;
    }
    /* Dos órdenes, misma entrada, salidas calientes; barrera por lote. */
    for (unsigned pass = 0; pass < 4; pass++) {
        unsigned vectorized = pass == 1 || pass == 2;
        uint64_t start = sysGetSystemTime();
        for (unsigned repeat = 0; repeat < 32; repeat++) {
            for (unsigned i = 0; i < 256; i++) {
                if (vectorized) rsx3d_vertex_vector(&out[i], &in[i], i*251+1);
                else rsx3d_vertex_scalar(&out[i], &in[i], i*251+1);
            }
            __asm__ volatile("" : : "r"(out) : "memory");
        }
        elapsed[vectorized] += sysGetSystemTime()-start;
    }
    salsads_log("DATO rsx3d banco vértices: 16384 vértices por ruta, escalar %llu us VMX %llu us (medir en PS3)",
                (unsigned long long)elapsed[0], (unsigned long long)elapsed[1]);
}
static unsigned hd_spe_tests(r3d_job *j)
{
    if (rsx3d_scale()==1) return 0;
    static gpu2d_frame f __attribute__((aligned(128)));
    gpu2d_scene_mem m; memset(&m,0,sizeof m);
    m.vram=vram; m.mmem=memalign(128,192*512);
    uint16_t *pixels=memalign(128,256*192*2);
    void *saved_meta=memalign(128,G2D_HD_BYTES+128);
    gpu2d_ctx *ppe_ctx=memalign(128,sizeof *ppe_ctx);
    uint16_t *ppe_pixels=memalign(128,256*192*2);
    unsigned fail=0;
    if (ppe_ctx) memset(ppe_ctx,0,sizeof *ppe_ctx);
    spukit_worker *worker=NULL;
    for (unsigned e=0;e<2;e++) {
        m.pal[e]=memalign(128,1024); m.oam[e]=memalign(128,1024);
        m.chg[e]=memalign(128,(GPU2D_SCENE_CHG_CAP+2)*sizeof(gpu2d_change));
        if (!m.pal[e] || !m.oam[e] || !m.chg[e]) fail=1;
    }
    if (!m.mmem || !pixels || !saved_meta || !ppe_ctx || !ppe_pixels || fail) { fail=1; goto done; }
    worker=spukit_worker_start(spu_gpu2d_bin,"hd-2d",0);
    if (!worker) { fail=1; goto done; }
    for (unsigned test=0;test<6;test++) {
        gpu2d_scene_build(&f,&m,test%GPU2D_SCENE_FIJAS,1+test);
        memset(&frame,0,sizeof frame); frame.regs.clear_depth=0x7fff;
        frame.regs.clear_color=((test%3==2?31:test%3*15)<<16)|0x3217;
        rsx3d_stats st;
        if (rsx3d_render(j,&st)!=1) { fail++; break; }
        gpu2d_engine *e=&f.eng[0];
        e->regs[G2D_R_DISPCNT] |= G2D_DC_3D | (1u<<8);
        e->img3d_ea=j->out_ea; e->out_ea=(uint64_t)(uintptr_t)pixels;
        e->hd_ea=rsx3d_hd_acquire(j->out_ea,0);
        if (!e->hd_ea) { fail++; break; }
        job_compose work={ (uint64_t)(uintptr_t)&f, 0 };
        uint32_t seq=spukit_post(worker,JOB_COMPOSE,&work,sizeof work);
        const spukit_result *res=spukit_wait(worker,seq);
        if (!res || res->error) { fail++; break; }
        gpu2d_ref_engine(e,(uint16_t *)(void *)reference);
        unsigned native_bad=0,meta_bad=0;
        const g2d_hd_pixel *meta=(const g2d_hd_pixel *)(uintptr_t)e->hd_ea;
        for (unsigned i=0;i<256*192;i++) {
            unsigned sx=(i%256+meta[i].b[11]+256u*meta[i].b[12])&511;
            uint32_t sample=sx<256?output[i/256*256+sx]:0;
            native_bad += pixels[i]!=((uint16_t *)(void *)reference)[i];
            meta_bad += pixels[i]!=gpu2d_to_5551(g2d_hd_eval(&meta[i],sample));
        }
        int shader_bad=rsx3d_hd_native_test(pixels);
        uint32_t fast_ticks=res->ticks;
        /* El exportador escalar por píxel vive en el PPE: referencia byte a byte del vectorial del SPE. */
        uint64_t spe_hd=e->hd_ea, scalar_us=0; unsigned scalar_bad=1;
        if (ppe_ctx) {
            memset(saved_meta,0,G2D_HD_BYTES);
            e->flags |= G2D_F_HD_SCALAR; e->hd_ea=(uint64_t)(uintptr_t)saved_meta; e->out_ea=(uint64_t)(uintptr_t)ppe_pixels;
            uint64_t t0=sysGetSystemTime();
            int rc=gpu2d_compose_engine(ppe_ctx,e);
            scalar_us=sysGetSystemTime()-t0;
            e->flags &= ~G2D_F_HD_SCALAR; e->hd_ea=spe_hd; e->out_ea=(uint64_t)(uintptr_t)pixels;
            scalar_bad=rc!=0 || memcmp(saved_meta,meta,G2D_HD_BYTES)!=0;
        }
        fail += native_bad || meta_bad || shader_bad || scalar_bad;
        salsads_log("rsx3d HD SPE caso %u: nativo %u metadatos %u shader %d diferencias; SPE vectorial %.3f ms; escalar PPE %.3f ms, paridad bytes %u",
            test,native_bad,meta_bad,shader_bad,fast_ticks*1000.0/spukit_tick_hz(),scalar_us/1000.0,scalar_bad);
    }
done:
    if (worker) spukit_worker_stop(worker);
    for (unsigned e=0;e<2;e++) { free(m.pal[e]); free(m.oam[e]); free(m.chg[e]); }
    free(m.mmem); free(pixels); free(saved_meta); free(ppe_ctx); free(ppe_pixels);
    return fail;
}

int rsx3d_selftest(void)
{
    unsigned fail = rsx3d_decode_test();
    uint32_t seed = 1; unsigned vertex_fail = 0;
    for (unsigned i = 0; i < 100000; i++) {
        gx_vertex g = {0}; rsx3d_vertex a, b;
        seed = seed * 1664525u + 1013904223u;
        g.sx = seed; g.sy = seed >> 16; g.z = seed & 0xffffff;
        g.s = seed >> 8; g.t = seed >> 4; g.r = seed & 63; g.g = (seed >> 6) & 63; g.b = (seed >> 12) & 63;
        uint32_t w = (i & 1) ? (seed & 0x7fffffffu) | 1u : (seed >> 16)+1;
        rsx3d_vertex_scalar(&a, &g, w);
        rsx3d_vertex_vector(&b, &g, w);
        vertex_fail += memcmp(&a, &b, sizeof a) != 0;
    }
    salsads_log("rsx3d vértices VMX: 100000 casos, %u fallos", vertex_fail);
    fail += vertex_fail;
    vertex_bench();
    r3d_job j; memset(&j, 0, sizeof j);
    j.frame_ea = (uint64_t)(uintptr_t)&frame; j.vram_ea = (uint64_t)(uintptr_t)vram;
    j.out_ea = (uint64_t)(uintptr_t)output;
    for (unsigned i = 0; i < 4; i++) j.texslot[i] = 0;
    for (unsigned i = 0; i < 6; i++) j.palslot[i] = R3D_UNMAPPED;
    for (unsigned i = 0; i < 64; i++) { vram[i*2] = 0xff; vram[i*2+1] = 0xff; }
    for (unsigned test = 0; test < 16; test++) {
        unsigned scene = test & 7;
        memset(&frame, 0, sizeof frame);
        frame.npolys = scene < 2 ? 1 : 3; frame.nverts = frame.npolys * 4;
        frame.swap_params = test < 8 ? 1 : 3; frame.regs.clear_depth = 0x7fff;
        frame.regs.disp3dcnt = D3_TEXTURE | D3_BLEND;
        if (scene & 1) frame.regs.clear_color = (31u << 16) | 31u;
        quad(0, 1, scene < 2 ? 31 : 15, 0, 63, 0, scene == 1);
        quad(1, scene < 4 ? 1 : 2, 15, 63, 0, 0, 0);
        quad(2, scene < 6 ? 1 : 2, 15, 0, 0, 63, 0);
        for (unsigned n = 0; n < frame.npolys; n++)
            for (unsigned k = 0; k < 4; k++) frame.vtx[n*4+k].w = 4096 - n*256;
        r3d_render_ref(&frame, vram, j.texslot, j.palslot, 0, reference);
        rsx3d_stats st; int ok = rsx3d_render(&j, &st);
        unsigned coverage = 0, color = 0, alpha = 0;
        for (unsigned k = 0; k < R3D_W * R3D_H; k++) {
            coverage += !!R3D_PX_A(output[k]) != !!R3D_PX_A(reference[k]);
            color += (output[k] & 0xffffffu) != (reference[k] & 0xffffffu);
            alpha += R3D_PX_A(output[k]) != R3D_PX_A(reference[k]);
        }
        unsigned center = 96 * R3D_W + 128;
        /* Centro plano: ninguna diferencia puede atribuirse a cobertura ni interpolación. */
        if (ok != 1 || coverage || color || alpha) fail++;
        salsads_log("rsx3d prueba %u: estado %d, centro %08x referencia %08x, cobertura %u color %u alfa %u, dibujos %u",
                    test, ok, output[center], reference[center], coverage, color, alpha, st.draws);
        if (test < 2) {
            unsigned bad = rsx3d_decode_raster_test(&j) || memcmp(output, reference, sizeof output);
            salsads_log("rsx3d alternancia RGBA8/raster %u: %u fallos", test, bad);
            fail += bad;
        }
    }
    fail += shade_tests(&j);
    /* Las expectativas históricas 33/35 conservan expresamente sus topes. */
    int capacity_saved=rsx3d_capacity_set_previous(1);
    fail += shadow_tests(&j);
    rsx3d_capacity_set_previous(capacity_saved);
    fail += equality_tests(&j);
    fail += fog_tests(&j,0);
    fail += fog_translucent_tests(&j,0);
    /* Niebla con sombras y con toon: un grupo que falle degrada solo esa
     * combinación a su respaldo SPE (como antes de admitirla) en vez de
     * apagar todo el 3D del RSX; el log lo anota y el veredicto lo cuenta. */
    unsigned degraded = 0;
    unsigned capacity_degraded=0;
    if (capacity_tests(&j,&fail)) {
        rsx3d_capacity_degrade(); capacity_degraded=1;
        salsads_log("rsx3d capacidad: ampliación degradada al SPE por diferencias exactas; se conservan los topes rc1");
    }
    if (fog_shadow_tests(&j)) { rsx3d_fog_degrade(0); degraded++; }
    if (fog_toon_tests(&j)) { rsx3d_fog_degrade(1); degraded++; }
    if (rsx3d_scale()>1) {
        fail += fog_tests(&j,1);
        fail += fog_translucent_tests(&j,1);
        fail += overlap_tests(&j,0,0,1);
        fail += reduced_sampling_tests(&j);
    }
    /* Los respaldos 4/9 comprueban los límites históricos de IDs. */
    capacity_saved=rsx3d_capacity_set_previous(1);
    fail += fallback_tests(&j);
    rsx3d_capacity_set_previous(capacity_saved);
    fail += w_tests(&j);
    blend_probe(&j);
    fail += cache_tests(&j);
    fail += cold_tests(&j);
    fail += cold_sampling_probe(&j);
    if (rsx3d_hd_experimental()) {
        /* Diagnóstico informativo: no abortar aunque aparezca atraso. */
        overlap_tests(&j,1,0,0);
        if (!rsx3d_hd_descriptors(1)) overlap_tests(&j,1,1,0);
        else salsads_log("rsx3d HD diagnóstico: sin memoria para descriptores distintos");
        rsx3d_hd_descriptors(0);
        /* Leer la composición introduce espera entre parejas: banco separado,
         * nunca sustituye el diagnóstico nativo sin esa espera. */
        overlap_tests(&j,2,0,0);
        if (!rsx3d_hd_descriptors(1)) overlap_tests(&j,2,1,0);
        rsx3d_hd_descriptors(0);
    } else fail += overlap_tests(&j,0,0,0);
    if (rsx3d_scale()>1) for (unsigned a=0;a<32;a++) {
        memset(&frame,0,sizeof frame); frame.regs.clear_depth=0x7fff;
        frame.regs.clear_color=(a<<16)|((a*13u)&31)|(((a*7u)&31)<<5)|(((a*3u)&31)<<10);
        rsx3d_stats st;
        fail += rsx3d_render(&j,&st)!=1;
        fail += rsx3d_hd_contract_test(j.out_ea,output[0]);
    }
    if (rsx3d_scale()>1) fail += rsx3d_hd_line_test(j.out_ea,output[0]);
    fail += hd_spe_tests(&j);
    fail += replay_tests(1);
    salsads_log("rsx3d autoprueba dirigida: %s, %u fallos, %u grupos de niebla y %u de capacidad degradados al SPE (la comparación de juegos y la mezcla RGBA8 son diagnósticos)",
                fail ? "FALLO" : degraded || capacity_degraded ? "OK con degradación" : "OK", fail, degraded,capacity_degraded);
    return fail ? -1 : 0;
}

int rsx3d_reference_test(void)
{
    return replay_tests(0) ? -1 : 0;
}
