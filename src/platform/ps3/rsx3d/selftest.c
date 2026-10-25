/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "platform/ps3/rsx3d/rsx3d.h"
#include "platform/ps3/log.h"
#include "gpu3d/r3d_px.h"
#include "gpu3d/rsx_replay.h"
#include "rsxkit/rsxkit.h"
#include "platform/ps3/autotest.h"
#include <sys/systime.h>
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
    const char *names[] = { "mkds", "sm64ds", "nsmb", "pesado" };
    int fail = 0;
    for (unsigned scene = 0; scene < 4; scene++) {
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
        rsx3d_stats st;
        memset(&st, 0, sizeof st);
        int native = enabled ? rsx3d_render(&j, &st) : 0;
        salsads_log("rsx3d reproducción %s: cuadro original %u, estado %d, %s, swap %u, disp3d %04x",
                    names[scene], frame.seq, native, rsx3d_reason(), frame.swap_params, frame.regs.disp3dcnt);
        if (native < 0) { fail++; continue; }
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
        for (unsigned variant = 0; variant < (scene == 3 ? 1u : 4u); variant++) {
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
    for (unsigned mode = 0; mode < 2; mode++) for (unsigned batch = 0; batch < 4; batch++) {
        memset(&frame, 0, sizeof frame);
        frame.npolys = 512; frame.nverts = 2048;
        frame.regs.clear_depth = 0x7fff; frame.regs.disp3dcnt = D3_TEXTURE | D3_BLEND;
        for (unsigned t = 0; t < 32; t++) {
            unsigned rgb = t | ((31-t) << 5) | (((t*7)&31) << 10) | 0x8000;
            vram[2*t] = rgb; vram[2*t+1] = rgb >> 8;
        }
        for (unsigned n = 0; n < 512; n++) {
            unsigned c = n & 63, t = batch*8 + n/64;
            unsigned x = (n%32)*8, y = (n/32)*12;
            quad(n, 1, 31, c, 63-c, (c*13)&63, 1);
            gx_poly *p = &frame.poly[n];
            p->attr |= mode << 4;
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
int rsx3d_selftest(void)
{
    unsigned fail = 0;
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
    }
    fail += shade_tests(&j);
    fail += w_tests(&j);
    blend_probe(&j);
    fail += cache_tests(&j);
    fail += replay_tests(1);
    salsads_log("rsx3d autoprueba dirigida: %s, %u fallos (la comparación de juegos y la mezcla RGBA8 son diagnósticos)", fail ? "FALLO" : "OK", fail);
    return fail ? -1 : 0;
}

int rsx3d_reference_test(void)
{
    return replay_tests(0) ? -1 : 0;
}
