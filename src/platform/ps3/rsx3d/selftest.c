/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <string.h>
#include "platform/ps3/rsx3d/rsx3d.h"
#include "platform/ps3/log.h"
#include "gpu3d/r3d_px.h"
#include "gpu3d/rsx_replay.h"
static gx_frame frame;
static uint8_t vram[R3D_VRAM_ZERO_OFF + 128] __attribute__((aligned(128)));
static uint32_t output[R3D_W * R3D_H] __attribute__((aligned(128)));
static uint32_t reference[R3D_W * R3D_H] __attribute__((aligned(128)));
static int replay_tests(void)
{
    const char *names[] = { "mkds", "sm64ds", "nsmb" };
    int fail = 0;
    for (unsigned scene = 0; scene < 3; scene++) {
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
        int native = rsx3d_render(&j, &st);
        salsads_log("rsx3d reproducción %s: cuadro original %u, estado %d, %s, swap %u, disp3d %04x",
                    names[scene], frame.seq, native, rsx3d_reason(), frame.swap_params, frame.regs.disp3dcnt);
        if (native < 0) { fail++; continue; }
        /* Diagnóstico explícito del subconjunto de fase 0, nunca usado en juego.
         * Ambas rutas reciben la misma lista filtrada y Z en lugar de W. */
        unsigned removed = 0;
        if (!native) {
            frame.swap_params &= ~2u;
            frame.regs.disp3dcnt &= ~(D3_FOG | D3_EDGE | D3_REARBMP);
            unsigned n = 0;
            for (unsigned i = 0; i < frame.npolys; i++) {
                gx_poly *p = &frame.poly[i];
                if (PA_MODE(p->attr) > 1 || !PA_ALPHA(p->attr) || (p->attr & PA_DEQUAL)) { removed++; continue; }
                frame.poly[n++] = *p;
            }
            frame.npolys = n;
        }
        r3d_render_ref(&frame, vram, j.texslot, j.palslot, j.flags, reference);
        int ok = rsx3d_render(&j, &st);
        if (ok != 1) { salsads_log("rsx3d reproducción %s: no dibujada, %s", names[scene], rsx3d_reason()); fail++; continue; }
        unsigned coverage = 0, color = 0, alpha = 0, maxerr = 0;
        snprintf(path, sizeof path, "/dev_hdd0/salsads/rsx3d-%s-diff.ppm", names[scene]);
        io = fopen(path, "wb");
        if (io) fprintf(io, "P6\n256 192\n255\n");
        for (unsigned k = 0; k < R3D_W * R3D_H; k++) {
            unsigned a = R3D_PX_A(output[k]), b = R3D_PX_A(reference[k]);
            unsigned cov = !!a != !!b;
            unsigned col = a && b && ((output[k] ^ reference[k]) & 0xffffffu);
            coverage += cov; color += !!col; alpha += a != b;
            if (a && b) for (unsigned c = 0; c < 3; c++) {
                int err = (int)((output[k] >> (8*c)) & 63u) - (int)((reference[k] >> (8*c)) & 63u);
                unsigned abserr = err < 0 ? -err : err;
                if (abserr > maxerr) maxerr = abserr;
            }
            if (io) { fputc(cov ? 255 : 0, io); fputc(col ? 255 : 0, io); fputc(a != b ? 255 : 0, io); }
        }
        if (io) fclose(io); else fail++;
        salsads_log("rsx3d reproducción %s: %s, omitidos %u, cobertura %u color %u alfa %u error RGB6 máximo %u; polígonos %u dibujos %u texturas %u bytes %u aciertos %u",
                    names[scene], native ? "original Z" : "subconjunto Z sin efectos", removed, coverage, color, alpha, maxerr,
                    st.polys, st.draws, st.textures, st.uploaded, st.cache_hits);
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
int rsx3d_selftest(void)
{
    unsigned fail = 0;
    r3d_job j; memset(&j, 0, sizeof j);
    j.frame_ea = (uint64_t)(uintptr_t)&frame; j.vram_ea = (uint64_t)(uintptr_t)vram;
    j.out_ea = (uint64_t)(uintptr_t)output;
    for (unsigned i = 0; i < 4; i++) j.texslot[i] = 0;
    for (unsigned i = 0; i < 6; i++) j.palslot[i] = R3D_UNMAPPED;
    for (unsigned i = 0; i < 64; i++) { vram[i*2] = 0xff; vram[i*2+1] = 0xff; }
    for (unsigned scene = 0; scene < 8; scene++) {
        memset(&frame, 0, sizeof frame);
        frame.npolys = scene < 2 ? 1 : 3; frame.nverts = frame.npolys * 4;
        frame.swap_params = 1; frame.regs.clear_depth = 0x7fff;
        frame.regs.disp3dcnt = D3_TEXTURE | D3_BLEND;
        if (scene & 1) frame.regs.clear_color = (31u << 16) | 31u;
        quad(0, 1, scene < 2 ? 31 : 15, 0, 63, 0, scene == 1);
        quad(1, scene < 4 ? 1 : 2, 15, 63, 0, 0, 0);
        quad(2, scene < 6 ? 1 : 2, 15, 0, 0, 63, 0);
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
        if (ok != 1 || output[center] != reference[center]) fail++;
        salsads_log("rsx3d prueba %u: estado %d, centro %08x referencia %08x, cobertura %u color %u alfa %u, dibujos %u",
                    scene, ok, output[center], reference[center], coverage, color, alpha, st.draws);
    }
    fail += replay_tests();
    salsads_log("rsx3d autoprueba: %s, %u fallos", fail ? "FALLO" : "OK", fail);
    return fail ? -1 : 0;
}
