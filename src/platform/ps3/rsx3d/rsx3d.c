/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Reproductor 1x. Listas y VRAM inmutables; una barrera al acabar el cuadro. */
#include <malloc.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/systime.h>
#include "platform/ps3/rsx3d/rsx3d.h"
#include "platform/ps3/log.h"
#include "gpu3d/r3d_px.h"
#include "rsxkit/rsxkit.h"
#include "rsxkit/post.h"
#include "shade_fpo.h"
#include "shade_w_fpo.h"
#include "pack_fpo.h"
#include "fog_fpo.h"
#include "fog_w_fpo.h"
#include "mask_fpo.h"
#include "mask_w_fpo.h"
#include "shadow_fpo.h"
#include "shadow_w_fpo.h"
#include "toon_fpo.h"
#include "toon_w_fpo.h"
#include "gpu3d/rsx_modes.h"

#define ARENA (24u * 1024u * 1024u)
#define TEX_BYTES (16u * 1024u * 1024u)
#define VERT_OFF TEX_BYTES
#define OUT_OFF (18u * 1024u * 1024u)
#define SNAP_OFF (19u * 1024u * 1024u)
#define TOON_OFF (20u * 1024u * 1024u)
#define CACHE_N 256u
#define PAGE_SIZE 1024u
#define PAGE_WORDS ((R3D_VRAM_ZERO_OFF / PAGE_SIZE + 31u) / 32u)
static uint8_t *mem;
static uint32_t offset, used, nc;
static rsxkit_target *target;
static rsxkit_depth *depth;
static rsxkit_surface *output;
static rsxkit_program *shade, *shade_w, *pack, *toon, *toon_w;
static int toon_spe, shadow_spe, fog_spe;
static rsxkit_program *fog, *fog_w;
static rsxkit_program *shadow, *shadow_w, *mask, *mask_w;
static uint8_t oid[64], tid[64];
static int shadows, single_shadow, shadow_general, shadow_unbatched;
static uint8_t trans_mask;
static uint32_t old_slots[10];
static int snapshot_valid;
static int ready;
static int unbatched, old_prepare, coarse_cache, profile;
static uint64_t tick_hz;
static uint64_t ticks(void)
{
    if (!profile) return 0;
#ifdef __powerpc__
    uint64_t t; __asm__ volatile("mftb %0" : "=r"(t) :: "memory"); return t;
#else
    return sysGetSystemTime();
#endif
}
static const char *reason = "sin inicializar";
static const r3d_job *cur;
static struct cache_entry {
    uint32_t key, pal, pages[PAGE_WORDS];
    int valid;
    rsxkit_texture tx;
} cache[CACHE_N];
static struct cache_entry *decoding;
static struct command { rsxkit_draw d; uint32_t first, count, id, tdepth, trans, mode; } command[GX_MAX_POLYS];
static uint16_t order[GX_MAX_POLYS];
static uint32_t keys[GX_MAX_POLYS];
static uint16_t sorted[GX_MAX_POLYS];
static rsxkit_draw last_draw;
static int last_valid;

const char *rsx3d_reason(void) { return reason; }
static uint32_t tex_read(void *unused, uint32_t a)
{
    (void)unused;
    uint32_t off = cur->texslot[(a >> 17) & 3u];
    if (off == R3D_UNMAPPED) return 0;
    off += a & 0x1ffffu;
    decoding->pages[off / PAGE_SIZE / 32u] |= 1u << (off / PAGE_SIZE % 32u);
    return ((const uint8_t *)(uintptr_t)cur->vram_ea)[off];
}
static uint32_t pal_read(void *unused, uint32_t a)
{
    (void)unused;
    uint32_t slot = (a >> 14) & 7u;
    uint32_t off = slot < 6 ? cur->palslot[slot] : R3D_UNMAPPED;
    if (off == R3D_UNMAPPED) return 0;
    off += a & 0x3fffu;
    decoding->pages[off / PAGE_SIZE / 32u] |= 1u << (off / PAGE_SIZE % 32u);
    return ((const uint8_t *)(uintptr_t)cur->vram_ea)[off];
}
static int texture(const r3d_tex *t, rsxkit_texture *out, rsx3d_stats *st)
{
    uint64_t t0 = ticks();
    struct cache_entry *entry = NULL;
    for (uint32_t i = 0; i < nc; i++) {
        if (cache[i].key == t->key && cache[i].pal == t->pkey) {
            entry = &cache[i];
            if (entry->valid) {
                *out = entry->tx; st->cache_hits++;
                st->lookup_us += ticks() - t0; goto wrap;
            }
            break;
        }
    }
    uint32_t w = 1u << t->sl, h = 1u << t->tl, pitch = (w * 4u + 63u) & ~63u;
    if (!entry && (nc == CACHE_N || used + pitch * h > TEX_BYTES)) return 0;
    st->lookup_us += ticks() - t0;
    uint32_t dest = entry ? entry->tx.offset - offset : used;
    if (!entry) { entry = &cache[nc++]; used = (used + pitch * h + 127u) & ~127u; }
    memset(entry->pages, 0, sizeof entry->pages);
    decoding = entry;
    t0 = ticks();
    memset(out, 0, sizeof *out);
    out->offset = offset + dest; out->pitch = pitch;
    out->width = w; out->height = h; out->format = RSXKIT_RGBA8888;
    out->location = RSXKIT_MEM_MAIN; out->filter = RSXKIT_FILTER_NEAREST;
    out->xf[0] = 1.0f / w; out->xf[1] = 1.0f / h;
    for (uint32_t y = 0; y < h; y++) for (uint32_t x = 0; x < w; x++) {
        uint32_t p = r3d_decode(t, x, y, tex_read, pal_read, NULL);
        uint8_t *b = mem + dest + y * pitch + x * 4u;
        b[0] = R3D_PX_R(p); b[1] = R3D_PX_G(p); b[2] = R3D_PX_B(p); b[3] = R3D_PX_A(p);
    }
    entry->key = t->key; entry->pal = t->pkey; entry->tx = *out; entry->valid = 1;
    st->decode_us += ticks() - t0;
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
    struct stat flag;
    unbatched = stat("/dev_hdd0/salsads/3d-rsx-sin-lotes", &flag) == 0;
    old_prepare = stat("/dev_hdd0/salsads/3d-rsx-preparacion-anterior", &flag) == 0;
    coarse_cache = stat("/dev_hdd0/salsads/3d-rsx-cache-gruesa", &flag) == 0;
    profile = stat("/dev_hdd0/salsads/3d-rsx-prueba", &flag) == 0 ||
              stat("/dev_hdd0/salsads/3d-rsx-perfil", &flag) == 0;
    fog_spe = stat("/dev_hdd0/salsads/3d-rsx-niebla-spe", &flag) == 0;
    shadow_unbatched = stat("/dev_hdd0/salsads/3d-rsx-sombra-sin-lotes", &flag) == 0;
    shadow_general = stat("/dev_hdd0/salsads/3d-rsx-sombra-general", &flag) == 0;
    shadow_spe = stat("/dev_hdd0/salsads/3d-rsx-sombra-spe", &flag) == 0;
    toon_spe = stat("/dev_hdd0/salsads/3d-rsx-toon-spe", &flag) == 0;
    tick_hz = sysGetTimebaseFrequency();
    memset(mem + OUT_OFF + R3D_W * R3D_H * 4u, 31, 512);
    if (rsxkit_map_main(mem, ARENA, &offset)) return -1;
    target = rsxkit_target_create_rgba(R3D_W, R3D_H);
    depth = rsxkit_depth_create(R3D_W, R3D_H);
    output = rsxkit_surface_wrap_main(R3D_W, R3D_H, RSXKIT_POST_RGBA8, mem + OUT_OFF, offset + OUT_OFF, R3D_W * 4);
    shade_w = rsxkit_program_create(shade_w_fpo);
    shade = rsxkit_program_create(shade_fpo); pack = rsxkit_program_create(pack_fpo);
    fog = rsxkit_program_create(fog_fpo); fog_w = rsxkit_program_create(fog_w_fpo);
    shadow = rsxkit_program_create(shadow_fpo); shadow_w = rsxkit_program_create(shadow_w_fpo);
    mask = rsxkit_program_create(mask_fpo); mask_w = rsxkit_program_create(mask_w_fpo);
    toon = rsxkit_program_create(toon_fpo); toon_w = rsxkit_program_create(toon_w_fpo);
    if (!target || !depth || !output || !shade || !shade_w || !pack || !toon || !toon_w || !shadow || !shadow_w || !mask || !mask_w || !fog || !fog_w) return -1;
    reason = "Z nativa; W por fragmento";
    ready = 1;
    return 0;
}
void rsx3d_shutdown(void)
{
    rsxkit_finish();
    rsxkit_program_destroy(fog); rsxkit_program_destroy(fog_w);
    rsxkit_program_destroy(shadow); rsxkit_program_destroy(shadow_w);
    rsxkit_program_destroy(mask); rsxkit_program_destroy(mask_w);
    rsxkit_program_destroy(toon); rsxkit_program_destroy(toon_w);
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
    if (!last_valid || memcmp(&last_draw, d, sizeof *d)) st->state_changes++;
    last_draw = *d; last_valid = 1;
    rsxkit_draw_triangles(d, c->first, c->count); st->draws++;
}
/* El RSX no necesita pendientes, área ni atributos de aristas del raster por líneas. */
static int prepare_poly(r3d_poly *p, const gx_poly *gp, const gx_vertex *const *vv, const r3d_rstate *rs)
{
    if (old_prepare) return r3d_poly_setup(p, gp, vv, rs);
    p->n = gp->nverts; p->attr = gp->attr;
    p->mode = PA_MODE(gp->attr); p->alpha = PA_ALPHA(gp->attr); p->id = PA_ID(gp->attr);
    p->wshift = gp->wshift;
    int ymin = vv[0]->sy, ymax = ymin;
    for (int i = 0; i < p->n; i++) {
        uint32_t w = (uint32_t)(vv[i]->w > 0 ? vv[i]->w : 1) >> p->wshift;
        p->v[i].w = w ? w : 1;
        if (vv[i]->sy < ymin) ymin = vv[i]->sy;
        if (vv[i]->sy > ymax) ymax = vv[i]->sy;
    }
    if (ymax < 0 || ymin >= R3D_H) return 0;
    uint32_t tp = gp->texparam, fmt = (tp >> 26) & 7u;
    p->textured = (rs->d3 & D3_TEXTURE) && fmt;
    if (p->textured) {
        r3d_tex *t = &p->tex;
        t->fmt = fmt; t->sl = 3u + ((tp >> 20) & 7u); t->tl = 3u + ((tp >> 23) & 7u);
        t->addr = (tp & 0xffffu) << 3; t->c0t = (tp >> 29) & 1u;
        t->rs = (tp >> 16) & 1u; t->rt = (tp >> 17) & 1u;
        t->fs = (tp >> 18) & 1u; t->ft = (tp >> 19) & 1u;
        t->pal = (gp->pltt & 0x1fffu) << (fmt == 2u ? 3 : 4);
        t->key = tp & 0x3ff0ffffu; t->pkey = fmt == 7u ? 0u : (gp->pltt & 0x1fffu);
    }
    return 1;
}
static int render_frame(const r3d_job *j, rsx3d_stats *st)
{
    const uint64_t start = sysGetSystemTime();
    const gx_frame *f = (const gx_frame *)(uintptr_t)j->frame_ea;
    r3d_rstate rs;
    uint32_t ncmd = 0, nv = 0;
    memset(st, 0, sizeof *st);
    last_valid = 0;
    if (!ready) { reason = "sin recursos"; return -1; }
    if (!f || (j->flags & R3D_F_BLANK)) { reason = "cuadro vacío"; return 0; }
    if (f->npolys > GX_MAX_POLYS || f->nverts > GX_MAX_VERTS) return -1;
    st->polys = f->npolys;
    r3d_rstate_init(&rs, f, j->flags);
    if (j->flags & R3D_F_NOFOG) rs.d3 &= ~D3_FOG;
    if (j->flags & R3D_F_NOEDGE) rs.d3 &= ~D3_EDGE;
    uint32_t modes = rsx_modes(f, j->flags);
    /* Registros habilitados sin efecto observable: no requieren otra pasada.
     * Densidad cero es identidad incluso con alfa/AND de flags translúcidos.
     * Sin cambio posible de OID, ningún vecino activa el marcado de bordes. */
    if ((modes & (1u << RSX_FOG)) && !fog_spe) {
        unsigned nonzero = 0;
        for (unsigned k = 0; k < 32; k++) nonzero |= rs.fog_table[k];
        if (!nonzero) { modes &= ~(1u << RSX_FOG); rs.d3 &= ~D3_FOG; }
    }
    if (modes & (1u << RSX_EDGE)) {
        unsigned changes = 0;
        for (unsigned i = 0; i < f->npolys; i++) {
            uint32_t a = f->poly[i].attr;
            changes |= PA_MODE(a) != 3 && PA_ALPHA(a) == 31 && PA_ID(a) != AT_OID(rs.clear_attr);
        }
        if (!changes) { modes &= ~(1u << RSX_EDGE); rs.d3 &= ~D3_EDGE; }
    }
    uint32_t supported = (1u << RSX_TOON) | (1u << RSX_HIGHLIGHT) |
                         (1u << RSX_SHADOW_MASK) | (1u << RSX_SHADOW) | (1u << RSX_FOG);
    if (fog_spe) supported &= ~(1u << RSX_FOG);
    if (toon_spe) supported &= ~((1u << RSX_TOON) | (1u << RSX_HIGHLIGHT));
    if (shadow_spe) supported &= ~((1u << RSX_SHADOW_MASK) | (1u << RSX_SHADOW));
    uint32_t unsupported = modes & ~supported;
    if (unsupported) {
        for (unsigned k = 0; k < RSX_MODE_COUNT; k++) if (unsupported & (1u << k)) {
            reason = rsx_mode_names[k]; break;
        }
        return 0;
    }
    if (modes & (1u << RSX_FOG)) {
        if (modes & ((1u << RSX_TOON) | (1u << RSX_HIGHLIGHT) | (1u << RSX_SHADOW_MASK) | (1u << RSX_SHADOW))) {
            reason = "niebla combinada con toon o sombra"; return 0;
        }
        for (unsigned i = 0; i < f->npolys; i++) {
            const gx_poly *p = &f->poly[i];
            unsigned fmt = (p->texparam >> 26) & 7u;
            if (rs.wbuf || (p->attr & PA_FOG)) {
                if (p->nverts < 3 || p->nverts > GX_POLY_MAXV || p->wshift > 28 || p->vidx[0] >= f->nverts) return -1;
                uint32_t w = rs.wbuf ? (uint32_t)f->vtx[p->vidx[0]].w : f->vtx[p->vidx[0]].z;
                for (unsigned k = 1; k < p->nverts; k++) {
                    if (p->vidx[k] >= f->nverts) return -1;
                    if ((rs.wbuf ? (uint32_t)f->vtx[p->vidx[k]].w : f->vtx[p->vidx[k]].z) != w) {
                        reason = "niebla Z/W interpolada: cuantización sin validar"; return 0;
                    }
                }
            }
            if (PA_ALPHA(p->attr) != 31 || (PA_MODE(p->attr) != 1 && (rs.d3 & D3_TEXTURE) && (fmt == 1 || fmt == 6))) {
                reason = "niebla con translúcidos: requiere pasada final"; return 0;
            }
        }
    }
    shadows = !!(modes & ((1u << RSX_SHADOW_MASK) | (1u << RSX_SHADOW)));
    if (shadows) {
        /* Si solo hay un ID de sombra basta guardar OID==sombra. El resto
         * de los IDs opacos son indistinguibles para las operaciones admitidas.
         * Cuatro bits TID: 0 vacío, 1..7 otros IDs, 8 sombra; bits 5/6/7
         * máscara, temporal y ocupado. Con varias sombras: OID2 + TID3. */
        unsigned shadow_id = 64;
        single_shadow = !shadow_general;
        for (unsigned i = 0; i < f->npolys; i++) {
            uint32_t a = f->poly[i].attr;
            if (PA_MODE(a) == 3 && PA_ID(a)) {
                if (shadow_id != 64 && shadow_id != PA_ID(a)) single_shadow = 0;
                shadow_id = PA_ID(a);
            }
        }
        if (shadow_id == 64) single_shadow = 0;
        memset(oid, 255, sizeof oid); memset(tid, 255, sizeof tid);
        unsigned no = 1, nt = 1;
        if (single_shadow) {
            for (unsigned id = 0; id < 64; id++) oid[id] = id == shadow_id;
            tid[shadow_id] = 16;
        } else oid[AT_OID(rs.clear_attr)] = 0;
        trans_mask = single_shadow ? 30 : 28;
        for (unsigned i = 0; i < f->npolys; i++) {
            const gx_poly *p = &f->poly[i];
            unsigned a = PA_ALPHA(p->attr), m = PA_MODE(p->attr), id = PA_ID(p->attr);
            unsigned fmt = (p->texparam >> 26) & 7u;
            if (m == 3 && !id) continue;
            /* LESS solo es equivalente si ningún escritor deja AT_BACK.
             * No confundir el empate frontal/trasero con igualdad explícita. */
            if (!(p->flags & GX_POLY_FRONT) && ((m != 3 && a == 31) || (p->attr & PA_TDEPTH))) {
                reason = "sombras: profundidad de cara trasera"; return 0;
            }
            if (m != 3 && a == 31 && oid[id] == 255) oid[id] = no++;
            if ((m == 3 || a < 31 || (m != 1 && (rs.d3 & D3_TEXTURE) && (fmt == 1 || fmt == 6))) && tid[id] == 255)
                tid[id] = nt++ << (single_shadow ? 1 : 2);
        }
        if (no > 4 || nt > 8) { reason = "sombras: capacidad de IDs excedida"; return 0; }
    }
    if (modes & ((1u << RSX_TOON) | (1u << RSX_HIGHLIGHT))) {
        for (unsigned i = 0; i < 32; i++) {
            uint8_t *b = mem + TOON_OFF + 4*i;
            b[0] = R3D_PX_R(rs.toon[i]); b[1] = R3D_PX_G(rs.toon[i]);
            b[2] = R3D_PX_B(rs.toon[i]); b[3] = 0;
        }
    }
    for (uint32_t i = 0; i < f->npolys; i++) {
        const gx_poly *p = &f->poly[i];
        if (p->nverts < 3 || p->nverts > GX_POLY_MAXV || p->wshift > 28) return -1;
        for (uint32_t k = 0; k < p->nverts; k++) if (p->vidx[k] >= f->nverts) return -1;
        uint32_t key = r3d_sort_key(p, f->swap_params & 1u), k = i;
        while (old_prepare && k && keys[k-1] > key) { keys[k] = keys[k-1]; order[k] = order[k-1]; k--; }
        keys[k] = key; order[k] = i;
    }
    /* Radix estable: conserva exactamente los empates y el orden DS. */
    if (!old_prepare) for (unsigned shift = 0; shift < 24; shift += 8) {
        uint32_t count[256] = {0}, sum = 0;
        for (uint32_t i = 0; i < f->npolys; i++) count[(keys[order[i]] >> shift) & 255u]++;
        for (unsigned i = 0; i < 256; i++) { uint32_t n = count[i]; count[i] = sum; sum += n; }
        for (uint32_t i = 0; i < f->npolys; i++) sorted[count[(keys[order[i]] >> shift) & 255u]++] = order[i];
        memcpy(order, sorted, f->npolys * sizeof *order);
    }
    uint64_t t0 = ticks();
    uint32_t slots[10]; memcpy(slots, j->texslot, 16); memcpy(slots + 4, j->palslot, 24);
    if (!snapshot_valid || memcmp(old_slots, slots, sizeof slots)) {
        memcpy(old_slots, slots, sizeof slots);
        memcpy(mem + SNAP_OFF, (const void *)(uintptr_t)j->vram_ea, R3D_VRAM_ZERO_OFF);
        used = nc = 0; snapshot_valid = 1;
    } else if (memcmp(mem + SNAP_OFF, (const void *)(uintptr_t)j->vram_ea, R3D_VRAM_ZERO_OFF)) {
        uint32_t changed[PAGE_WORDS] = {0};
        for (uint32_t a = 0; a < R3D_VRAM_ZERO_OFF; a += PAGE_SIZE) {
            const uint8_t *src = (const uint8_t *)(uintptr_t)j->vram_ea + a;
            if (memcmp(mem + SNAP_OFF + a, src, PAGE_SIZE)) {
                changed[a / PAGE_SIZE / 32u] |= 1u << (a / PAGE_SIZE % 32u);
                memcpy(mem + SNAP_OFF + a, src, PAGE_SIZE);
            }
        }
        for (uint32_t i = 0; i < nc; i++) for (unsigned w = 0; w < PAGE_WORDS; w++) {
            if (coarse_cache ? changed[w] != 0 : (changed[w] & cache[i].pages[w]) != 0) {
                if (coarse_cache) { used = nc = 0; break; }
                cache[i].valid = 0; break;
            }
        }
    }
    st->identity_us = ticks() - t0;
    cur = j;
    rsxkit_vertex *v = (rsxkit_vertex *)(void *)(mem + VERT_OFF);
    for (uint32_t i = 0; i < f->npolys; i++) {
        t0 = ticks();
        const gx_poly *gp = &f->poly[order[i]];
        const gx_vertex *vv[GX_POLY_MAXV]; r3d_poly p = {0};
        for (uint32_t k = 0; k < gp->nverts; k++) vv[k] = &f->vtx[gp->vidx[k]];
        if (!prepare_poly(&p, gp, vv, &rs)) continue;
        struct command *c = &command[ncmd++]; memset(c, 0, sizeof *c);
        c->mode = p.mode; c->first = nv; c->count = (p.n - 2) * 3; c->trans = p.alpha < 31 || (p.mode != 1 && p.textured && (p.tex.fmt == 1 || p.tex.fmt == 6)); c->id = p.id; c->tdepth = !!(p.attr & PA_TDEPTH);
        rsxkit_draw *d = &c->d; d->program = rs.wbuf ? shade_w : shade; d->ntex = 1;
        d->scissor[2] = R3D_W; d->scissor[3] = R3D_H;
        d->k[0][0][0] = p.textured; d->k[0][0][1] = p.alpha; d->k[0][0][2] = p.mode == 3 ? 0 : p.mode;
        d->k[1][0][1] = (float)(1u << p.wshift) / 16777215.0f;
        d->k[1][0][0] = (rs.d3 & D3_ALPHATEST) ? rs.alpha_ref : 0;
        if (p.mode == 3) d->program = rs.wbuf ? shadow_w : shadow;
        if (p.mode == 2) {
            d->tex[1].offset = offset + TOON_OFF; d->tex[1].pitch = 128;
            d->tex[1].width = 32; d->tex[1].height = 1;
            d->tex[1].format = RSXKIT_RGBA8888; d->tex[1].location = RSXKIT_MEM_MAIN;
            d->tex[1].filter = RSXKIT_FILTER_NEAREST;
            d->tex[1].wrap_s = d->tex[1].wrap_t = RSXKIT_WRAP_CLAMP;
        }
        if (rs.d3 & D3_FOG) {
            d->program = rs.wbuf ? fog_w : fog;
            /* Densidad entera antes del interpolador: ni Z normalizada ni
             * constantes grandes deben decidir un borde de la tabla. */
            uint32_t fog_depth = rs.wbuf ? p.v[0].w << p.wshift : vv[0]->z;
            if (fog_depth > 0xffffffu) fog_depth = 0xffffffu;
            d->k[3][0][3] = (p.attr & PA_FOG) ? r3d_fog_density(&rs, fog_depth) : 0;
            uint32_t fc = rs.fog_px;
            d->k[2][0][0] = R3D_PX_R(fc); d->k[2][0][1] = R3D_PX_G(fc);
            d->k[2][0][2] = R3D_PX_B(fc); d->k[2][0][3] = R3D_PX_A(fc);
            d->k[3][0][2] = !!(rs.d3 & D3_FOG_ALPHA);
        }
        if (p.mode == 2) {
            d->program = rs.wbuf ? toon_w : toon; d->ntex = 2;
            d->k[1][0][2] = !!(rs.d3 & D3_HIGHLIGHT);
        }
        st->vertex_us += ticks() - t0;
        /* Sin textura: una textura blanca de 8x8 propia, colocada fuera de la arena. */
        if (p.textured) {
            if (!texture(&p.tex, &d->tex[0], st)) { reason = "arena de texturas llena"; used = nc = 0; return 0; }
        } else {
            if (old_prepare) memset(mem + OUT_OFF + R3D_W * R3D_H * 4u, 31, 512);
            d->tex[0].offset = offset + OUT_OFF + R3D_W * R3D_H * 4u;
            d->tex[0].pitch = 64; d->tex[0].width = d->tex[0].height = 8;
            d->tex[0].location = RSXKIT_MEM_MAIN; d->tex[0].format = RSXKIT_RGBA8888;
            d->tex[0].filter = RSXKIT_FILTER_NEAREST;
        }
        t0 = ticks();
        rsxkit_vertex pv[GX_POLY_MAXV];
        int count = old_prepare ? (p.n - 2) * 3 : p.n;
        for (int a = 0; a < count; a++) {
            int ix = old_prepare ? (a % 3 ? a / 3 + a % 3 : 0) : a;
            const gx_vertex *g = vv[ix]; rsxkit_vertex *b = old_prepare ? &v[nv++] : &pv[ix];
            float w = (float)p.v[ix].w;
            memset(b, 0, sizeof *b);
            b->pos[0] = g->sx * w; b->pos[1] = g->sy * w;
            b->pos[2] = (g->z / 16777215.0f) * w; b->pos[3] = w;
            b->st[0] = g->s / 16.0f; b->st[1] = g->t / 16.0f;
            b->rgba[0] = g->r; b->rgba[1] = g->g; b->rgba[2] = g->b; b->rgba[3] = 255;
        }
        if (!old_prepare) for (int k = 1; k + 1 < p.n; k++) {
            v[nv++] = pv[0]; v[nv++] = pv[k]; v[nv++] = pv[k+1];
        }
        st->vertex_us += ticks() - t0;
    }
    st->polys = ncmd;
    uint32_t batches = 0;
    for (uint32_t i = 0; i < ncmd; i++) {
        struct command *c = &command[i];
        st->draws_before += shadows ? (c->mode == 3 ? (c->id ? (single_shadow ? 3u : 6u + (oid[c->id] != 255)) : 1u) :
                                                    (c->trans ? 6u : 1u)) : (c->trans ? 5u : 1u);
        struct command *prev = batches ? &command[batches-1] : NULL;
        /* No mover primitivas: ni empates Z/W ni IDs repetidos cambian ganador.
         * Las cinco pasadas translúcidas permanecen juntas por polígono. */
        if (!unbatched && prev &&
            ((!prev->trans && !c->trans && prev->mode != 3 && c->mode != 3) ||
             (prev->mode == 3 && c->mode == 3 &&
              ((!prev->id && !c->id) || (single_shadow && !shadow_unbatched &&
               c->id && !c->d.k[0][0][0] && c->d.k[0][0][1] > c->d.k[1][0][0])))) && prev->id == c->id &&
            prev->tdepth == c->tdepth && prev->first + prev->count == c->first &&
            !memcmp(&prev->d, &c->d, sizeof c->d)) prev->count += c->count;
        else command[batches++] = *c;
    }
    ncmd = batches;
    t0 = ticks();
    rsxkit_texture_sync();
    st->upload_us = ticks() - t0;
    st->identity_us = st->identity_us * 1000000u / tick_hz;
    st->lookup_us = st->lookup_us * 1000000u / tick_hz;
    st->decode_us = st->decode_us * 1000000u / tick_hz;
    st->vertex_us = st->vertex_us * 1000000u / tick_hz;
    st->upload_us = st->upload_us * 1000000u / tick_hz;
    st->prepare_us = sysGetSystemTime() - start;
    if (rsxkit_bind(target, depth, R3D_W, R3D_H)) return -1;
    uint32_t clear = rs.clear_px;
    if ((rs.d3 & D3_FOG) && (rs.clear_attr & AT_FOG)) {
        uint32_t d = r3d_fog_density(&rs, rs.clear_depth), inv = 128-d, fc = rs.fog_px;
        uint32_t r = R3D_PX_R(clear), g = R3D_PX_G(clear), b = R3D_PX_B(clear);
        if (!(rs.d3 & D3_FOG_ALPHA)) {
            r = (r*inv + R3D_PX_R(fc)*d) >> 7;
            g = (g*inv + R3D_PX_G(fc)*d) >> 7;
            b = (b*inv + R3D_PX_B(fc)*d) >> 7;
        }
        clear = R3D_PX(r,g,b,(R3D_PX_A(clear)*inv + R3D_PX_A(fc)*d) >> 7);
    }
    uint32_t ca = R3D_PX_A(clear), encoded_a = ca ? ((ca + 1u) * 255u + 16u) / 32u : 0;
    uint32_t encoded = (encoded_a << 24) | ((R3D_PX_R(clear) + 16u) << 16) |
        ((R3D_PX_G(clear) + 16u) << 8) | (R3D_PX_B(clear) + 16u);
    rsxkit_stamp(56);
    rsxkit_clear(RSXKIT_CLEAR_COLOR | RSXKIT_CLEAR_DEPTH | RSXKIT_CLEAR_ALPHA, encoded, rs.clear_depth / 16777215.0f, NULL);
    rsxkit_clear_stencil(shadows ? ((ca ? 128 : 0) | oid[AT_OID(rs.clear_attr)]) : (ca ? 64 : 255));
    rsxkit_vertices(offset + VERT_OFF, RSXKIT_MEM_MAIN);
    for (uint32_t i = 0; i < ncmd; i++) {
        struct command *c = &command[i];
        uint8_t z = RSXKIT_DEPTH_TEST | RSXKIT_DEPTH_LESS;
        if (shadows) {
            rsxkit_program *color_program = c->d.program;
            uint8_t label = tid[c->id];
            if (c->mode == 3) {
                c->d.program = rs.wbuf ? mask_w : mask;
                if (!c->id) {
                    c->d.stencil_zfail = 1;
                    draw(c, 1, 32, 255, 32, 2, RSXKIT_BLEND_NONE, z, 0, st);
                    c->d.stencil_zfail = 0;
                } else if (single_shadow) {
                    /* Bits 7/5/4/0: ocupado, máscara, último ID de sombra,
                     * OID igual a sombra. La comparación rechaza ambos IDs;
                     * invertir publica el TID sin exigirlo antes de dibujar.
                     * Los bits bajos del TID anterior ya no se consultan hasta
                     * que otro translúcido los reemplaza (y limpia el bit 4). */
                    /* Consumo inmediato: la siguiente primitiva del lote
                     * ve la máscara apagada también si falla profundidad.
                     * Solo sin descarte alfa/textura; los demás conservan
                     * las tres pasadas originales por polígono. */
                    int consume = !shadow_unbatched && !c->d.k[0][0][0] &&
                                  c->d.k[0][0][1] > c->d.k[1][0][0];
                    c->d.stencil_zfail = consume ? 2 : 0;
                    c->d.program = color_program;
                    draw(c, 3, 160, 177, consume ? 48 : 16, 1,
                         (rs.d3 & D3_BLEND) ? RSXKIT_BLEND_ALPHA_MAX : RSXKIT_BLEND_OFF,
                         z | (c->tdepth ? RSXKIT_DEPTH_WRITE : 0), (rs.d3 & D3_BLEND) ? 1 : 2, st);
                    /* Un borrado con alfa ocupa toda la pantalla; ninguna
                     * pasada admitida vuelve a dejar un destino vacío. */
                    if (!consume || !ca)
                        draw(c, 3, 32, 177, consume ? 176 : 144, 1, RSXKIT_BLEND_OFF,
                             z | (c->tdepth ? RSXKIT_DEPTH_WRITE : 0), 2, st);
                    c->d.stencil_zfail = 0;
                    c->d.program = rs.wbuf ? mask_w : mask;
                    draw(c, 1, 0, 0, 32, 0, RSXKIT_BLEND_NONE, 0, 0, st);
                } else {
                    /* Consumir la máscara aun si falla Z, ID o alfa. El bit 6
                     * solo vive durante este polígono; OID nunca se pierde. */
                    draw(c, 3, 96, 32, 64, 0, RSXKIT_BLEND_NONE, 0, 0, st);
                    if (oid[c->id] != 255)
                        draw(c, 3, oid[c->id], 3, 64, 0, RSXKIT_BLEND_NONE, 0, 0, st);
                    draw(c, 3, label, trans_mask, 64, 0, RSXKIT_BLEND_NONE, 0, 0, st);
                    c->d.program = color_program;
                    draw(c, 3, label | 192, 192, trans_mask | 128, 0,
                         (rs.d3 & D3_BLEND) ? RSXKIT_BLEND_ALPHA_MAX : RSXKIT_BLEND_OFF,
                         z | (c->tdepth ? RSXKIT_DEPTH_WRITE : 0), (rs.d3 & D3_BLEND) ? 1 : 2, st);
                    draw(c, 3, label | 64, 192, trans_mask, 0, RSXKIT_BLEND_OFF,
                         z | (c->tdepth ? RSXKIT_DEPTH_WRITE : 0), 2, st);
                    draw(c, 3, label | 128, trans_mask, 128, 0, RSXKIT_BLEND_NONE, 0, 2, st);
                    c->d.program = rs.wbuf ? mask_w : mask;
                    draw(c, 1, 0, 0, 96, 0, RSXKIT_BLEND_NONE, 0, 0, st);
                }
                c->d.program = color_program;
                continue;
            }
            draw(c, 1, oid[c->id] | 128, 255, 159, 0, RSXKIT_BLEND_OFF, z | RSXKIT_DEPTH_WRITE, 0, st);
            if (!c->trans) continue;
            draw(c, 3, label, single_shadow && label == 16 ? 16 : trans_mask, 64, 1, RSXKIT_BLEND_NONE, z, 2, st);
            draw(c, 3, label | 128, 192, trans_mask | 128, 0,
                 (rs.d3 & D3_BLEND) ? RSXKIT_BLEND_ALPHA_MAX : RSXKIT_BLEND_OFF,
                 z | (c->tdepth ? RSXKIT_DEPTH_WRITE : 0), (rs.d3 & D3_BLEND) ? 1 : 2, st);
            draw(c, 3, label, 128, trans_mask, 0, RSXKIT_BLEND_OFF,
                 z | (c->tdepth ? RSXKIT_DEPTH_WRITE : 0), 2, st);
            draw(c, 3, label | 128, trans_mask, 128, 0, RSXKIT_BLEND_NONE, 0, 2, st);
            draw(c, 3, 64, 64, 64, 1, RSXKIT_BLEND_NONE, 0, 2, st);
            continue;
        }
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
    st->raster_ns = rsxkit_stamp_ns(57) - rsxkit_stamp_ns(56);
    st->export_ns = rsxkit_stamp_ns(58) - rsxkit_stamp_ns(57);
    reason = rs.wbuf ? "W por fragmento; interpolación triangular" : "Z nativa; cobertura e interpolación triangular";
    return 1;
}

int rsx3d_render(const r3d_job *j, rsx3d_stats *st)
{
    static unsigned frames, rendered, counts[RSX_MODE_COUNT];
    int result = render_frame(j, st);
    const gx_frame *f = (const gx_frame *)(uintptr_t)j->frame_ea;
    uint32_t modes = rsx_modes(f, j->flags);
    rendered += result > 0;
    for (unsigned k = 0; k < RSX_MODE_COUNT; k++) counts[k] += (modes >> k) & 1u;
    if (++frames == 120) {
        salsads_log("DATO rsx3d censo: cuadros %u modo RSX %u SPE %u; imagen-borrado %u niebla %u bordes %u toon %u resaltado %u mascara-sombra %u sombra %u igualdad %u alambre %u",
                    frames, rendered, frames-rendered, counts[0], counts[1], counts[2], counts[3],
                    counts[4], counts[5], counts[6], counts[7], counts[8]);
        frames = rendered = 0; memset(counts, 0, sizeof counts);
    }
    return result;
}
