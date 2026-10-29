/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "check.h"
#include "gpu3d/rsx_export.h"
#include "gpu3d/rsx_replay.h"
#include "gpu3d/rsx_modes.h"
#include "gpu3d/rsx_vertex.h"

#include "core/nds_priv.h"
#include <stdlib.h>
/* La misma función del SPE con DMA de memoria: límites bajo ASan/UBSan. */
static void decode_get(void *dst, uint64_t src, uint32_t n, uint32_t tag)
{ (void)tag; memcpy(dst, (const void *)(uintptr_t)src, n); }
static void decode_put(const void *src, uint64_t dst, uint32_t n, uint32_t tag)
{ (void)tag; memcpy((void *)(uintptr_t)dst, src, n); }
static void decode_wait(uint32_t tags) { (void)tags; }
#define spukit_dma_get decode_get
#define spukit_dma_put decode_put
#define spukit_dma_wait decode_wait
#include "spu/rsxprep/decode.h"
#undef spukit_dma_get
#undef spukit_dma_put
#undef spukit_dma_wait
static uint32_t check_decode_tex(void *v, uint32_t a)
{
    const rsx_decode_job *j = v; uint32_t off = j->texslot[(a >> 17) & 3];
    return off == R3D_UNMAPPED ? 0 : ((const uint8_t *)(uintptr_t)j->vram_ea)[off+(a&0x1ffff)];
}
static uint32_t check_decode_pal(void *v, uint32_t a)
{
    const rsx_decode_job *j = v; unsigned slot = (a >> 14)&7;
    uint32_t off = slot < 6 ? j->palslot[slot] : R3D_UNMAPPED;
    return off == R3D_UNMAPPED ? 0 : ((const uint8_t *)(uintptr_t)j->vram_ea)[off+(a&0x3fff)];
}
static void decode_portable(void)
{
    static uint8_t vr[R3D_VRAM_ZERO_OFF], pixels[64*512+128];
    static rsx_decode_local local;
    static rsx_decode_item d;
    rsx_decode_job j = {0};
    j.items_ea = (uint64_t)(uintptr_t)&d; j.vram_ea = (uint64_t)(uintptr_t)vr; j.count = 1;
    for (unsigned i = 0; i < sizeof vr; i++) vr[i] = (i*73+(i>>8))&255;
    for (unsigned i = 0; i < 4; i++) j.texslot[i] = i*0x20000;
    for (unsigned i = 0; i < 6; i++) j.palslot[i] = 0x80000+i*0x4000;
    for (unsigned fmt = 1; fmt <= 7; fmt++) for (unsigned unmapped = 0; unmapped < 2; unmapped++) {
        memset(&d, 0, sizeof d); memset(pixels, 0xcd, sizeof pixels);
        d.tex.fmt = fmt; d.tex.sl = 7; d.tex.tl = 6; d.tex.addr = 0x1ff80;
        d.tex.pal = 0x3ff0; d.tex.c0t = unmapped; d.pitch = 512;
        d.out_ea = (uint64_t)(uintptr_t)pixels;
        j.texslot[1] = unmapped ? R3D_UNMAPPED : 0x20000;
        j.palslot[1] = unmapped ? R3D_UNMAPPED : 0x84000;
        for (unsigned part = 0; part < 2; part++) {
            d.first = part*32; d.rows = 32;
            CHECK_EQ(rsx_decode_run(&local, &j), 0);
        }
        for (unsigned y = 0; y < 64; y++) for (unsigned x = 0; x < 128; x++) {
            uint32_t p = r3d_decode(&d.tex, x, y, check_decode_tex, check_decode_pal, &j);
            const uint8_t *b = pixels+y*512+x*4;
            CHECK(b[0] == R3D_PX_R(p) && b[1] == R3D_PX_G(p) && b[2] == R3D_PX_B(p) && b[3] == R3D_PX_A(p));
        }
        for (unsigned k = 64*512; k < sizeof pixels; k++) CHECK_EQ(pixels[k], 0xcd);
    }
    d.first = 63; d.rows = 2;
    CHECK_EQ(rsx_decode_run(&local, &j), -1);
    d.first = 0; d.rows = 64; d.pitch = 511;
    CHECK_EQ(rsx_decode_run(&local, &j), -1);
}

static void replay_roundtrip(void)
{
    static gx_frame f;
    static uint8_t vram[R3D_VRAM_ZERO_OFF];
    r3d_job j;
    memset(&f, 0, sizeof f); memset(&j, 0, sizeof j);
    f.npolys = 1; f.nverts = 3; f.swap_params = 3;
    f.vtx[0].sx = -1; f.vtx[1].s = -32768; f.vtx[2].w = 0x1234567;
    f.poly[0].nverts = 3; f.poly[0].vidx[1] = 1; f.poly[0].vidx[2] = 2;
    f.regs.toon_table[31] = 0x1234; j.palslot[5] = R3D_UNMAPPED;
    vram[123] = 0xab;
    FILE *io = tmpfile();
    CHECK(io != NULL);
    if (!io) return;
    CHECK(r3d_replay_io(io, &f, &j, vram, 0));
    rewind(io);
    CHECK_EQ(fgetc(io), 'R'); CHECK_EQ(fgetc(io), 'S'); CHECK_EQ(fgetc(io), 'X'); CHECK_EQ(fgetc(io), '1');
    rewind(io);
    memset(&f, 0, sizeof f); memset(&j, 0, sizeof j); memset(vram, 0, sizeof vram);
    CHECK(r3d_replay_io(io, &f, &j, vram, 1));
    CHECK_EQ(f.npolys, 1); CHECK_EQ(f.nverts, 3); CHECK_EQ(f.swap_params, 3);
    CHECK(f.vtx[0].sx == -1); CHECK(f.vtx[1].s == -32768); CHECK_EQ(f.vtx[2].w, 0x1234567);
    CHECK_EQ(f.poly[0].vidx[2], 2); CHECK_EQ(f.regs.toon_table[31], 0x1234);
    CHECK_EQ(j.palslot[5], R3D_UNMAPPED); CHECK_EQ(vram[123], 0xab);
    CHECK(!r3d_replay_io(io, &f, &j, vram, 1));
    fclose(io);
}
static void mode_census(void)
{
    static gx_frame f;
    memset(&f, 0, sizeof f);
    CHECK_EQ(rsx_modes(NULL, 0), 0);
    CHECK_EQ(rsx_modes(&f, 0), 0);
    f.regs.disp3dcnt = D3_REARBMP | D3_FOG | D3_EDGE;
    f.npolys = 4;
    f.poly[0].attr = (2u << 4) | (31u << 16);
    f.poly[1].attr = (3u << 4) | (31u << 16);
    f.poly[2].attr = (3u << 4) | (31u << 16) | (1u << 24);
    f.poly[3].attr = PA_DEQUAL;
    uint32_t all = ((1u << RSX_MODE_COUNT)-1u) & ~(1u << RSX_HIGHLIGHT);
    CHECK_EQ(rsx_modes(&f, 0), all);
    CHECK_EQ(rsx_modes(&f, R3D_F_BLANK), 0);
    CHECK_EQ(rsx_modes(&f, R3D_F_NOEDGE | R3D_F_NOFOG), all & ~((1u << RSX_FOG) | (1u << RSX_EDGE)));
    f.regs.disp3dcnt |= D3_HIGHLIGHT;
    CHECK_EQ(rsx_modes(&f, 0), (all & ~(1u << RSX_TOON)) | (1u << RSX_HIGHLIGHT));
    CHECK(strcmp(rsx_mode_names[RSX_SHADOW], rsx_mode_names[RSX_SHADOW_MASK]) != 0);
}

static void snapshot_identity(void)
{
    struct nds *n = calloc(1, sizeof *n);
    CHECK(n != NULL);
    if (!n) return;
    nds_video_init(n);
    uint64_t old = n->vid.set[n->vid.cur].vram_version[0];
    /* Alternar los dos juegos sin escritura no cambia la identidad. */
    for (unsigned k = 0; k < 4; k++) {
        nds_video_frame_begin(n);
        CHECK(n->vid.set[n->vid.cur].vram_version[0] == old);
    }
    n->vram[0x4000] = 73;
    nds_video_vram_dirty(n, 0x4000);
    nds_video_vram_dirty(n, 0x7fff);
    nds_video_frame_begin(n);
    uint64_t changed = n->vid.set[n->vid.cur].vram_version[1];
    CHECK(changed != old);
    CHECK(n->vid.set[n->vid.cur].vram_version[0] == old);
    CHECK_EQ(n->vid.set[n->vid.cur].vram[0x4000], 73);
    nds_video_frame_begin(n);
    CHECK(n->vid.set[n->vid.cur].vram_version[1] == changed);
    CHECK_EQ(n->vid.set[n->vid.cur].vram[0x4000], 73);
    nds_video_init(n);
    CHECK(n->vid.set[n->vid.cur].vram_version[0] != old);
    CHECK(n->vid.set[n->vid.cur].vram_version[1] != changed);
    free(n);
}
/* El backend aplazado publica la capa únicamente al recibir wait3d. */
struct delayed_3d { struct nds *n; unsigned waits, submits; int set; };
static void delayed_wait(void *ctx, int set)
{
    struct delayed_3d *d = ctx;
    d->waits++; d->set = set;
    for (unsigned i = 0; i < R3D_W * R3D_H; i++)
        d->n->vid.set[set].img3d[i] = R3D_PX(63, 0, 0, 31);
}
static void delayed_compose(void *ctx, int set, gpu2d_frame *f)
{
    struct delayed_3d *d = ctx;
    (void)f;
    CHECK(d->waits > 0); CHECK_EQ(set, d->set);
    CHECK_EQ(d->n->vid.set[set].img3d[0], R3D_PX(63, 0, 0, 31));
    d->submits++;
}
static void delayed_consumers(void)
{
    struct nds *n = calloc(1, sizeof *n);
    CHECK(n != NULL); if (!n) return;
    struct delayed_3d d = {n, 0, 0, -1};
    n->vid.ops.ctx = &d; n->vid.ops.wait3d = delayed_wait;
    n->vid.ops.submit = delayed_compose;
    n->vid.r3d_defer_line = 160;
    n->vid.last = 1; n->vid.submit_pending = 1;
    nds_video_submit_pending(n, 262); nds_video_submit_pending(n, 159);
    CHECK_EQ(d.waits, 0); CHECK_EQ(d.submits, 0);
    nds_video_submit_pending(n, 160);
    CHECK_EQ(d.waits, 1); CHECK_EQ(d.submits, 1); CHECK_EQ(d.set, 1);
    /* DISPCAPCNT no espera al siguiente cuadro: leer VRAM ve el resultado. */
    n->vid.cur = 0; n->vid.recording = 1;
    struct nds_vset *v = &n->vid.set[0];
    v->has3d = 1; v->frame.eng[0].img3d_ea = (uint64_t)(uintptr_t)v->img3d;
    v->frame.eng[0].regs[G2D_R_DISPCAP + 1] = 0x8130;
    n->io[0].gen[G2D_R_DISPCAP + 1] = 0x8130;
    nds_vramcnt_write(n, 0, 0x80);
    nds_video_frame_end(n);
    CHECK_EQ(d.waits, 2); CHECK_EQ(d.set, 0);
    CHECK_EQ(n->vram[0], 0x1f); CHECK_EQ(n->vram[1], 0x80);
    CHECK_EQ(n->vram[256 * 192 * 2 - 2], 0x1f);
    CHECK_EQ(n->vram[256 * 192 * 2 - 1], 0x80);
    CHECK_EQ(n->io[0].gen[G2D_R_DISPCAP + 1] & 0x8000, 0);
    free(n);
}
void test_rsx_export(void)
{
    /* Incluye negativos, extremos, W normalizada y Z no múltiplo de 512. */
    uint32_t seed = 1;
    for (unsigned i = 0; i < 100000; i++) {
        gx_vertex g = {0}; rsx3d_vertex a, b;
        seed = seed * 1664525u + 1013904223u;
        g.sx = seed; g.sy = seed >> 16; g.z = seed & 0xffffffu;
        g.s = seed >> 8; g.t = seed >> 4;
        g.r = seed & 63; g.g = (seed >> 6) & 63; g.b = (seed >> 12) & 63;
        uint32_t w = (i & 1) ? (seed & 0x7fffffffu) | 1u : (seed >> 16) + 1;
        rsx3d_vertex_scalar(&a, &g, w); rsx3d_vertex_vector(&b, &g, w);
        CHECK(!memcmp(&a, &b, sizeof a));
    }
    decode_portable();
    delayed_consumers();
    snapshot_identity();
    replay_roundtrip();
    mode_census();
    uint8_t b[4];
    /* Los cuatro canales recorren su dominio, con valores vecinos distintos. */
    for (unsigned a = 0; a < 32; a++) for (unsigned r = 0; r < 64; r++)
    for (unsigned g = 0; g < 64; g++) {
        uint32_t p = R3D_PX(r, g, (r + g + a) & 63, a);
        r3d_rsx_pack(b, p);
        CHECK_EQ(b[0], a); CHECK_EQ(b[1], (r + g + a) & 63);
        CHECK_EQ(b[2], g); CHECK_EQ(b[3], r);
        CHECK_EQ(r3d_rsx_unpack(b), p);
    }
}
