/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reproductor 1x. Listas y VRAM inmutables; una barrera al acabar el cuadro. */
#include <malloc.h>
#include <string.h>
#include <sys/systime.h>
#include "platform/ps3/rsx3d/rsx3d.h"
#include "gpu3d/r3d_px.h"
#include "rsxkit/rsxkit.h"
#include "rsxkit/post.h"
#include "shade_fpo.h"
#include "shade_w_fpo.h"
#include "pack_fpo.h"

#define ARENA (24u * 1024u * 1024u)
#define TEX_BYTES (16u * 1024u * 1024u)
#define VERT_OFF TEX_BYTES
#define OUT_OFF (18u * 1024u * 1024u)
#define SNAP_OFF (19u * 1024u * 1024u)
#define CACHE_N 256u
static uint8_t *mem;
static uint32_t offset, used, nc;
static rsxkit_target *target;
static rsxkit_depth *depth;
static rsxkit_surface *output;
static rsxkit_program *shade, *shade_w, *pack;
static uint32_t old_slots[10];
static int snapshot_valid;
static int ready;
static const char *reason = "sin inicializar";
static const r3d_job *cur;
static struct cache_entry { uint32_t key, pal; rsxkit_texture tx; } cache[CACHE_N];
static struct command { rsxkit_draw d; uint32_t first, count, id, tdepth, trans; } command[GX_MAX_POLYS];
static uint16_t order[GX_MAX_POLYS];
static uint32_t keys[GX_MAX_POLYS];

const char *rsx3d_reason(void) { return reason; }
static uint32_t tex_read(void *unused, uint32_t a)
{
    (void)unused;
    uint32_t off = cur->texslot[(a >> 17) & 3u];
    return off == R3D_UNMAPPED ? 0 : ((const uint8_t *)(uintptr_t)cur->vram_ea)[off + (a & 0x1ffffu)];
}
static uint32_t pal_read(void *unused, uint32_t a)
{
    (void)unused;
    uint32_t slot = (a >> 14) & 7u;
    uint32_t off = slot < 6 ? cur->palslot[slot] : R3D_UNMAPPED;
    return off == R3D_UNMAPPED ? 0 : ((const uint8_t *)(uintptr_t)cur->vram_ea)[off + (a & 0x3fffu)];
}
static int texture(const r3d_tex *t, rsxkit_texture *out, rsx3d_stats *st)
{
    for (uint32_t i = 0; i < nc; i++) {
        if (cache[i].key == t->key && cache[i].pal == t->pkey) {
            *out = cache[i].tx; st->cache_hits++; goto wrap;
        }
    }
    uint32_t w = 1u << t->sl, h = 1u << t->tl, pitch = (w * 4u + 63u) & ~63u;
    if (nc == CACHE_N || used + pitch * h > TEX_BYTES) return 0;
    memset(out, 0, sizeof *out);
    out->offset = offset + used; out->pitch = pitch;
    out->width = w; out->height = h; out->format = RSXKIT_RGBA8888;
    out->location = RSXKIT_MEM_MAIN; out->filter = RSXKIT_FILTER_NEAREST;
    out->xf[0] = 1.0f / w; out->xf[1] = 1.0f / h;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint32_t p = r3d_decode(t, x, y, tex_read, pal_read, NULL);
        uint8_t *b = mem + used + y * pitch + x * 4u;
        b[0] = R3D_PX_R(p); b[1] = R3D_PX_G(p); b[2] = R3D_PX_B(p); b[3] = R3D_PX_A(p);
    }
    cache[nc].key = t->key; cache[nc].pal = t->pkey; cache[nc++].tx = *out;
    used = (used + pitch * h + 127u) & ~127u;
    st->textures++; st->uploaded += pitch * h;
wrap:
    out->wrap_s = t->rs ? (t->fs ? RSXKIT_WRAP_MIRROR : RSXKIT_WRAP_REPEAT) : RSXKIT_WRAP_CLAMP;
    out->wrap_t = t->rt ? (t->ft ? RSXKIT_WRAP_MIRROR : RSXKIT_WRAP_REPEAT) : RSXKIT_WRAP_CLAMP;
    return 1;
}
int rsx3d_init(void)
{
    if (mem) return ready ? 0 : -1;
    mem = memalign(1024u * 1024u, ARENA);
    if (!mem) return -1;
    memset(mem, 0, ARENA);
    if (rsxkit_map_main(mem, ARENA, &offset)) return -1;
    target = rsxkit_target_create_rgba(R3D_W, R3D_H);
    depth = rsxkit_depth_create(R3D_W, R3D_H);
    output = rsxkit_surface_wrap_main(R3D_W, R3D_H, RSXKIT_POST_RGBA8, mem + OUT_OFF, offset + OUT_OFF, R3D_W * 4);
    shade_w = rsxkit_program_create(shade_w_fpo);
    shade = rsxkit_program_create(shade_fpo); pack = rsxkit_program_create(pack_fpo);
    if (!target || !depth || !output || !shade || !shade_w || !pack) return -1;
    reason = "Z nativa; W por fragmento";
    ready = 1;
    return 0;
}
void rsx3d_shutdown(void)
{
    rsxkit_finish();
    rsxkit_program_destroy(shade_w); rsxkit_program_destroy(shade); rsxkit_program_destroy(pack);
    rsxkit_surface_destroy(output); rsxkit_target_destroy(target); rsxkit_depth_destroy(depth);
    ready = 0;
    /* La memoria mapeada se conserva hasta cerrar rsxkit; no reutilizar un mapeo vivo. */
}
static void draw(struct command *c, uint8_t test, uint8_t ref, uint8_t compare, uint8_t write, uint8_t op,
                 uint8_t blend, uint8_t z, float pass, rsx3d_stats *st)
{
    rsxkit_draw *d = &c->d;
    d->stencil = test; d->stencil_ref = ref; d->stencil_compare_mask = compare;
    d->stencil_write_mask = write; d->stencil_op = op;
    d->blend = blend; d->depth = z; d->k[0][0][3] = pass;
    rsxkit_draw_triangles(d, c->first, c->count); st->draws++;
}
int rsx3d_render(const r3d_job *j, rsx3d_stats *st)
{
    const uint64_t start = sysGetSystemTime();
    const gx_frame *f = (const gx_frame *)(uintptr_t)j->frame_ea;
    r3d_rstate rs;
    uint32_t ncmd = 0, nv = 0;
    memset(st, 0, sizeof *st);
    if (!ready) { reason = "sin recursos"; return -1; }
    if (!f || (j->flags & R3D_F_BLANK)) { reason = "cuadro vacío"; return 0; }
    if (f->npolys > GX_MAX_POLYS || f->nverts > GX_MAX_VERTS) return -1;
    st->polys = f->npolys;
    r3d_rstate_init(&rs, f, j->flags);
    if (rs.d3 & (D3_FOG | D3_EDGE | D3_REARBMP)) {
        reason = "niebla, bordes o imagen de borrado"; return 0;
    }
    for (uint32_t i = 0; i < f->npolys; i++) {
        const gx_poly *p = &f->poly[i];
        if (PA_MODE(p->attr) > 1 || !PA_ALPHA(p->attr) || (p->attr & PA_DEQUAL)) {
            reason = "toon, sombra, alambre o igualdad"; return 0;
        }
        if (p->nverts < 3 || p->nverts > GX_POLY_MAXV || p->wshift > 28) return -1;
        for (uint32_t k = 0; k < p->nverts; k++) if (p->vidx[k] >= f->nverts) return -1;
        uint32_t key = r3d_sort_key(p, f->swap_params & 1u), k = i;
        while (k && keys[k-1] > key) { keys[k] = keys[k-1]; order[k] = order[k-1]; k--; }
        keys[k] = key; order[k] = i;
    }
    /* Identidad completa, sin fiarse de una huella: incluye todos los bancos y ambos mapas.
     * Es deliberadamente gruesa: una escritura ajena a texturas también invalida. */
    uint32_t slots[10]; memcpy(slots, j->texslot, 16); memcpy(slots + 4, j->palslot, 24);
    if (!snapshot_valid || memcmp(old_slots, slots, sizeof slots) ||
        memcmp(mem + SNAP_OFF, (const void *)(uintptr_t)j->vram_ea, R3D_VRAM_ZERO_OFF)) {
        memcpy(old_slots, slots, sizeof slots);
        memcpy(mem + SNAP_OFF, (const void *)(uintptr_t)j->vram_ea, R3D_VRAM_ZERO_OFF);
        used = nc = 0; snapshot_valid = 1;
    }
    cur = j;
    rsxkit_vertex *v = (rsxkit_vertex *)(void *)(mem + VERT_OFF);
    for (uint32_t i = 0; i < f->npolys; i++) {
        const gx_poly *gp = &f->poly[order[i]];
        const gx_vertex *vv[GX_POLY_MAXV]; r3d_poly p = {0};
        for (uint32_t k = 0; k < gp->nverts; k++) vv[k] = &f->vtx[gp->vidx[k]];
        if (!r3d_poly_setup(&p, gp, vv, &rs)) continue;
        struct command *c = &command[ncmd++]; memset(c, 0, sizeof *c);
        c->first = nv; c->count = (p.n - 2) * 3; c->trans = p.alpha < 31 || (p.mode == 0 && p.textured && (p.tex.fmt == 1 || p.tex.fmt == 6)); c->id = p.id; c->tdepth = !!(p.attr & PA_TDEPTH);
        rsxkit_draw *d = &c->d; d->program = rs.wbuf ? shade_w : shade; d->ntex = 1;
        d->scissor[2] = R3D_W; d->scissor[3] = R3D_H;
        d->k[0][0][0] = p.textured; d->k[0][0][1] = p.alpha; d->k[0][0][2] = p.mode;
        d->k[1][0][1] = (float)(1u << p.wshift) / 16777215.0f;
        d->k[1][0][0] = (rs.d3 & D3_ALPHATEST) ? rs.alpha_ref : 0;
        /* Sin textura: una textura blanca de 8x8 propia, colocada fuera de la arena. */
        if (p.textured) {
            if (!texture(&p.tex, &d->tex[0], st)) { reason = "arena de texturas llena"; used = nc = 0; return 0; }
        } else {
            uint8_t *white = mem + OUT_OFF + R3D_W * R3D_H * 4u;
            memset(white, 31, 512); d->tex[0].offset = offset + OUT_OFF + R3D_W * R3D_H * 4u;
            d->tex[0].pitch = 64; d->tex[0].width = d->tex[0].height = 8;
            d->tex[0].location = RSXKIT_MEM_MAIN; d->tex[0].format = RSXKIT_RGBA8888;
            d->tex[0].filter = RSXKIT_FILTER_NEAREST;
        }
        for (int k = 1; k + 1 < p.n; k++) for (int a = 0; a < 3; a++) {
            int ix = a == 0 ? 0 : a == 1 ? k : k + 1;
            const gx_vertex *g = vv[ix]; rsxkit_vertex *b = &v[nv++];
            float w = (float)p.v[ix].w;
            memset(b, 0, sizeof *b);
            b->pos[0] = g->sx * w; b->pos[1] = g->sy * w;
            b->pos[2] = (g->z / 16777215.0f) * w; b->pos[3] = w;
            b->st[0] = g->s / 16.0f; b->st[1] = g->t / 16.0f;
            b->rgba[0] = g->r; b->rgba[1] = g->g; b->rgba[2] = g->b; b->rgba[3] = 255;
        }
    }
    rsxkit_texture_sync();
    st->prepare_us = sysGetSystemTime() - start;
    if (rsxkit_bind(target, depth, R3D_W, R3D_H)) return -1;
    uint32_t clear = rs.clear_px;
    uint32_t ca = R3D_PX_A(clear), encoded_a = ca ? ((ca + 1u) * 255u + 16u) / 32u : 0;
    uint32_t encoded = (encoded_a << 24) | ((R3D_PX_R(clear) + 16u) << 16) |
        ((R3D_PX_G(clear) + 16u) << 8) | (R3D_PX_B(clear) + 16u);
    rsxkit_stamp(56);
    rsxkit_clear(RSXKIT_CLEAR_COLOR | RSXKIT_CLEAR_DEPTH | RSXKIT_CLEAR_ALPHA, encoded, rs.clear_depth / 16777215.0f, NULL);
    rsxkit_clear_stencil(ca ? 64 : 255);
    rsxkit_vertices(offset + VERT_OFF, RSXKIT_MEM_MAIN);
    for (uint32_t i = 0; i < ncmd; i++) {
        struct command *c = &command[i];
        uint8_t z = RSXKIT_DEPTH_TEST | RSXKIT_DEPTH_LESS;
        draw(c, 1, 64, 255, 255, 0, RSXKIT_BLEND_OFF, z | RSXKIT_DEPTH_WRITE, 0, st);
        if (!c->trans) continue;
        /* Marcar repetidos, mezclar ocupados, reemplazar vacíos y restaurar el marcador.
         * 0..63 ID translúcido, 64 opaco, 255 vacío; bit 7 temporal de repetición. */
        draw(c, 3, c->id, 255, 128, 1, RSXKIT_BLEND_NONE, z, 2, st);
        draw(c, 3, c->id, 128, 255, 0, (rs.d3 & D3_BLEND) ? RSXKIT_BLEND_ALPHA_MAX : RSXKIT_BLEND_OFF,
             z | (c->tdepth ? RSXKIT_DEPTH_WRITE : 0), (rs.d3 & D3_BLEND) ? 1 : 2, st);
        draw(c, 2, c->id, 127, 255, 0, RSXKIT_BLEND_OFF, z | (c->tdepth ? RSXKIT_DEPTH_WRITE : 0), 2, st);
        draw(c, 3, c->id, 127, 128, 0, RSXKIT_BLEND_NONE, 0, 2, st);
    }
    rsxkit_stamp(57);
    rsxkit_texture tx; rsxkit_target_texture(target, 0, R3D_H, &tx);
    rsxkit_post_pass pass; memset(&pass, 0, sizeof pass);
    pass.program = pack; pass.out = output; pass.inputs = 1;
    pass.in[0] = rsxkit_post_texture_input(&tx, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
    if (rsxkit_post_run(&pass)) return -1;
    rsxkit_stamp(58); rsxkit_fence_wait(rsxkit_fence());
    if (rsxkit_lost()) return -1;
    uint64_t copy_start = sysGetSystemTime();
    memcpy((void *)(uintptr_t)j->out_ea, mem + OUT_OFF, R3D_W * R3D_H * 4u);
    st->copy_us = sysGetSystemTime() - copy_start;
    st->total_us = sysGetSystemTime() - start;
    st->polys = ncmd; st->raster_ns = rsxkit_stamp_ns(57) - rsxkit_stamp_ns(56);
    st->export_ns = rsxkit_stamp_ns(58) - rsxkit_stamp_ns(57);
    reason = rs.wbuf ? "W por fragmento; interpolación triangular" : "Z nativa; cobertura e interpolación triangular";
    return 1;
}
