/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Render 1x: dos exportaciones pendientes, retiradas antes de consumir el 3D. */
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
#include "compose_hd_fpo.h"
#include "probe_hd_fpo.h"
#include "gpu2d/gpu2d_hd.h"
#include "fog_fpo.h"
#include "fog_w_fpo.h"
#include "mask_fpo.h"
#include "mask_w_fpo.h"
#include "shadow_fpo.h"
#include "shadow_w_fpo.h"
#include "toon_fpo.h"
#include "toon_w_fpo.h"
#include "gpu3d/rsx_modes.h"
#include "gpu3d/rsx_vertex.h"
#include "spu/rsxprep/job.h"
#include "spu/raster3d/job.h"
#include <spukit/spukit.h>
#include "spu_rsxprep_bin.h"
_Static_assert(sizeof(rsx3d_vertex) == sizeof(rsxkit_vertex), "formato de vértices RSX");

#define ARENA (24u * 1024u * 1024u)
#define TEX_BYTES (16u * 1024u * 1024u)
#define VERT_OFF(slot) ((slot) ? 21u * 1024u * 1024u : TEX_BYTES)
#define OUT_OFF(slot) (18u * 1024u * 1024u + (slot) * 256u * 1024u)
#define SNAP_OFF (19u * 1024u * 1024u)
#define TOON_OFF(slot) (20u * 1024u * 1024u + (slot) * 128u)
#define CACHE_N 256u
_Static_assert(GX_MAX_POLYS * (GX_POLY_MAXV - 2u) * 3u * sizeof(rsxkit_vertex) <= 2u * 1024u * 1024u, "vértices por ranura");
#define PAGE_SIZE 1024u
#define PAGE_WORDS ((R3D_VRAM_ZERO_OFF / PAGE_SIZE + 31u) / 32u)
_Static_assert(RSX_DECODE_PAGES == PAGE_WORDS, "máscara de páginas compartida");
static uint8_t *mem;
static uint32_t offset, used, nc;
static rsxkit_target *target;
static unsigned hd_scale = 1;
static int hd_refresh = 1;
static rsxkit_target *hd_target[2], *hd_saved[2];
static rsxkit_depth *hd_depth;
static uint8_t *hd_mem;
static uint32_t hd_offset;
static uint64_t hd_native[2];
static int hd_valid[2];
static uint32_t hd_present_fence[2];
static uint64_t hd_present_ns, hd_compose_ns, hd_copy_ns, hd_raster_ns, hd_metadata_us;
static int hd_composed[2], hd_copied[2];
static unsigned hd_samples;
static rsxkit_program *hd_program;
#define HD_STRIDE (1024u * 1024u)
unsigned rsx3d_scale(void) { return hd_scale; }
void rsx3d_hd_invalidate(uint64_t ea)
{
    for (unsigned i=0;i<2;i++) if (hd_native[i]==ea) hd_native[i]=0;
}
uint64_t rsx3d_hd_acquire(uint64_t ea, unsigned set)
{
    if (hd_scale == 1 || set > 1) return 0;
    if (hd_present_fence[set]) {
        rsxkit_fence_wait(hd_present_fence[set]);
        hd_present_ns += rsxkit_stamp_ns(41+set*2)-rsxkit_stamp_ns(40+set*2);
        if (hd_composed[set]) hd_compose_ns += rsxkit_stamp_ns(45+set*2)-rsxkit_stamp_ns(44+set*2);
        if (hd_copied[set]) hd_copy_ns += rsxkit_stamp_ns(53+set*2)-rsxkit_stamp_ns(52+set*2);
        hd_present_fence[set]=0;
        if (++hd_samples == 120) {
            salsads_log("DATO rsx3d HD costos: escala %ux; copia local %.3f composición HD %.3f presentación completa %.3f ms; publicación PPE %.3f ms por cuadro (%u muestras)",hd_scale,hd_copy_ns/120000000.0,hd_compose_ns/120000000.0,hd_present_ns/120000000.0,hd_metadata_us/120000.0,hd_samples);
            hd_samples=0; hd_present_ns=hd_compose_ns=hd_copy_ns=hd_metadata_us=0;
        }
    }
    hd_valid[set] = hd_copied[set] = 0;
    for (unsigned i=0;i<2;i++) if (ea && hd_native[i]==ea) {
        rsxkit_stamp(52+set*2);
        if (rsxkit_target_copy(hd_saved[set], hd_target[i])) return 0;
        rsxkit_stamp(53+set*2); hd_copied[set]=1;
        hd_valid[set] = 1;
        return (uint64_t)(uintptr_t)(hd_mem + set*HD_STRIDE);
    }
    return 0;
}
static uint64_t hd_compose_pass(rsxkit_post_pass *out, unsigned set, int x, int y, int w, int h)
{
    /* Publicar una copia nativa inmutable: el productor DMA y la textura
     * mantienen direcciones distintas hasta la etiqueta de presentación. */
    uint64_t start = sysGetSystemTime();
    memcpy(hd_mem+(2+set)*HD_STRIDE,hd_mem+set*HD_STRIDE,G2D_HD_BYTES);
    __sync_synchronize();
    rsxkit_texture tx; rsxkit_target_texture(hd_saved[set], 0, R3D_H*hd_scale, &tx);
    rsxkit_post_pass pass; memset(&pass, 0, sizeof pass);
    pass.program = hd_program; pass.inputs=2;
    pass.rect = (rsxkit_rect){x,y,w,h}; pass.step[0]=256.0f/w; pass.step[1]=192.0f/h;
    pass.k[0][0] = hd_scale;
    pass.in[0]=rsxkit_post_texture_input(&tx, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
    memset(&tx,0,sizeof tx); tx.offset=hd_offset+(2+set)*HD_STRIDE;
    tx.width=1024; tx.height=192; tx.pitch=4096;
    tx.format=RSXKIT_ARGB8888; tx.location=RSXKIT_MEM_MAIN;
    pass.in[1]=rsxkit_post_texture_input(&tx, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
    pass.in[1].refresh = hd_refresh;
    *out = pass;
    return sysGetSystemTime()-start;
}
int rsx3d_hd_present(unsigned set, int x, int y, int w, int h)
{
    if (hd_scale == 1 || set > 1) return 0;
    static unsigned count, shown, missing, modes, errors;
    const char *why = !hd_valid[set] ? "sin raster del cuadro" :
        *(uint32_t *)(void *)(hd_mem + set*HD_STRIDE + G2D_HD_BYTES) ? "modo de pantalla no gráfico" : NULL;
    if (why) { if (!hd_valid[set]) missing++; else modes++; }
    else {
        rsxkit_post_pass pass; hd_metadata_us += hd_compose_pass(&pass,set,x,y,w,h);
        rsxkit_stamp(44+set*2);
        if (rsxkit_post_run(&pass)) why="error de composición";
        else shown++;
        rsxkit_stamp(45+set*2);
        hd_composed[set]=1;
        if (why) errors++;
    }
    if (++count == 120) {
        salsads_log("DATO rsx3d HD: escala %ux; HD %u; nativa por raster ausente %u, modo no gráfico %u, error %u", hd_scale, shown, missing, modes, errors);
        count=shown=missing=modes=errors=0;
    }
    return why == NULL;
}
void rsx3d_presentation_begin(unsigned set)
{
    if (hd_scale == 1 || set > 1) return;
    hd_composed[set]=0;
    rsxkit_stamp(40+set*2);
}
void rsx3d_presentation_end(unsigned set)
{
    if (hd_scale == 1 || set > 1) return;
    rsxkit_stamp(41+set*2);
    hd_present_fence[set]=rsxkit_fence();
}
static rsxkit_depth *depth;
static rsxkit_surface *output[2];
static struct {
    uint32_t fence;
    uint64_t out_ea;
    rsx3d_stats stats;
    int pending;
} flight[2];
static uint32_t texture_fence;
static rsxkit_program *shade, *shade_w, *pack, *toon, *toon_w;
static int toon_spe, shadow_spe, fog_spe;
static rsxkit_program *fog, *fog_w;
static rsxkit_program *shadow, *shadow_w, *mask, *mask_w;
static uint8_t oid[64], tid[64];
static int shadows, single_shadow, shadow_general, shadow_unbatched;
static uint8_t trans_mask;
static uint32_t old_slots[10];
static int snapshot_valid;
static int full_identity, versions_valid;
static uint64_t version_ea, old_versions[41];
static const uint64_t *next_versions;
void rsx3d_identity_versions(uint64_t vram_ea, const uint64_t *versions)
{
    version_ea = vram_ea; next_versions = versions;
}
static int ready;
static int unbatched, old_prepare, coarse_cache, profile, scalar_vertices;
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
    uint32_t rows, wanted;
    rsxkit_texture tx;
} cache[CACHE_N];
/* Parámetros fríos separados de las claves consultadas por cada polígono. */
static r3d_tex cache_source[CACHE_N];
static spukit_worker *decode_worker;
static int decode_owned, decode_ppe, decode_missing, decode_error;
static uint32_t decode_seq, decode_count, decode_index[CACHE_N];
static rsx_decode_item decode_items[CACHE_N] __attribute__((aligned(128)));
static rsx_decode_job decode_job;
static unsigned cold_frames, busy_frames, decode_batches;
static uint64_t decode_ticks, decode_pixels;
static int decode_check(const rsx_decode_item *d, const rsx_decode_job *job);
static int decode_verify;
/* La instantánea y las entradas no se modifican hasta retirar este lote. */
static int decode_collect(void)
{
    if (!decode_seq) return 1;
    if (!spukit_done(decode_worker, decode_seq)) { busy_frames++; return 0; }
    const spukit_result *r = spukit_wait(decode_worker, decode_seq);
    decode_seq = 0;
    if (!r || r->error) { decode_error = 1; return 0; }
    decode_ticks += r->ticks;
    for (unsigned i = 0; i < decode_count; i++) {
        decode_pixels += decode_items[i].rows * (1u << decode_items[i].tex.sl);
        if (decode_verify && decode_check(&decode_items[i], &decode_job)) { decode_error = 1; return 0; }
        struct cache_entry *e = &cache[decode_index[i]];
        for (unsigned k = 0; k < PAGE_WORDS; k++) e->pages[k] |= decode_items[i].pages[k];
        e->rows += decode_items[i].rows;
        e->valid = e->rows == e->tx.height;
    }
    decode_count = 0;
    return 1;
}
void rsx3d_decode_kick(void)
{
    if (!decode_worker || decode_ppe || decode_seq || !decode_count || decode_error) return;
    decode_seq = spukit_post(decode_worker, JOB_RSX_RGBA8, &decode_job, sizeof decode_job);
    decode_batches++;
}
void rsx3d_decode_drain(void)
{
    rsx3d_decode_kick();
    if (decode_seq) spukit_wait(decode_worker, decode_seq);
}
void rsx3d_decode_worker(spukit_worker *worker)
{
    if (decode_seq) { spukit_wait(decode_worker, decode_seq); decode_collect(); }
    if (decode_owned) spukit_worker_stop(decode_worker);
    decode_owned = 0; decode_worker = worker;
    decode_count = 0;
}
static void decode_plan(void)
{
    unsigned remaining = RSX_DECODE_PIXELS;
    decode_count = 0;
    for (unsigned k = 0; k < nc && remaining; k++) {
        struct cache_entry *e = &cache[k];
        if (!e->wanted || e->valid) continue;
        unsigned rows = remaining / e->tx.width;
        if (rows > e->tx.height - e->rows) rows = e->tx.height - e->rows;
        if (!rows) continue;
        rsx_decode_item *d = &decode_items[decode_count];
        memset(d, 0, sizeof *d); d->tex = cache_source[k];
        d->out_ea = (uint64_t)(uintptr_t)(mem + (e->tx.offset - offset));
        d->pitch = e->tx.pitch; d->first = e->rows; d->rows = rows;
        decode_index[decode_count++] = k;
        remaining -= rows * e->tx.width;
    }
    decode_job.items_ea = (uint64_t)(uintptr_t)decode_items;
    decode_job.vram_ea = (uint64_t)(uintptr_t)(mem + SNAP_OFF);
    decode_job.count = decode_count;
    memcpy(decode_job.texslot, old_slots, 16); memcpy(decode_job.palslot, old_slots+4, 24);
}
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
static int decode_check(const rsx_decode_item *d, const rsx_decode_job *job)
{
    r3d_job j = {0}; j.vram_ea = job->vram_ea;
    memcpy(j.texslot, job->texslot, 16); memcpy(j.palslot, job->palslot, 24);
    const r3d_job *saved = cur; struct cache_entry *saved_decoding = decoding;
    struct cache_entry check = {0}; cur = &j; decoding = &check;
    unsigned fail = 0;
    for (unsigned y = d->first; y < d->first+d->rows; y++) for (unsigned x = 0; x < (1u << d->tex.sl); x++) {
        uint32_t p = r3d_decode(&d->tex, x, y, tex_read, pal_read, NULL);
        const uint8_t *b = (const uint8_t *)(uintptr_t)d->out_ea + y*d->pitch+x*4;
        fail += b[0] != R3D_PX_R(p) || b[1] != R3D_PX_G(p) || b[2] != R3D_PX_B(p) || b[3] != R3D_PX_A(p);
    }
    fail += memcmp(check.pages, d->pages, sizeof check.pages) != 0;
    cur = saved; decoding = saved_decoding;
    if (fail) salsads_log("FALLO RGBA8 SPE: formato %u filas %u+%u diferencias %u", d->tex.fmt, d->first, d->rows, fail);
    return fail != 0;
}
int rsx3d_decode_test(void)
{
    if (!decode_worker || decode_ppe) return 0;
    unsigned fail = 0;
    rsx_decode_job j = {0};
    j.items_ea = (uint64_t)(uintptr_t)decode_items;
    j.vram_ea = (uint64_t)(uintptr_t)(mem+SNAP_OFF); j.count = 1;
    for (unsigned a = 0; a < R3D_VRAM_ZERO_OFF; a++) mem[SNAP_OFF+a] = (a*73u+(a>>8)) & 255;
    for (unsigned i = 0; i < 4; i++) j.texslot[i] = i*0x20000u;
    for (unsigned i = 0; i < 6; i++) j.palslot[i] = 0x80000u+i*0x4000u;
    for (unsigned fmt = 1; fmt <= 7; fmt++) for (unsigned unmapped = 0; unmapped < 2; unmapped++) {
        rsx_decode_item *d = decode_items; memset(d, 0, sizeof *d);
        d->tex.fmt = fmt; d->tex.sl = 7; d->tex.tl = 6; d->tex.addr = 0x1ff80;
        d->tex.pal = 0x3ff0; d->tex.c0t = unmapped;
        d->out_ea = (uint64_t)(uintptr_t)mem; d->pitch = 512; d->first = 3; d->rows = 61;
        j.texslot[1] = unmapped ? R3D_UNMAPPED : 0x20000;
        j.palslot[1] = unmapped ? R3D_UNMAPPED : 0x84000;
        uint32_t seq = spukit_post(decode_worker, JOB_RSX_RGBA8, &j, sizeof j);
        const spukit_result *r = spukit_wait(decode_worker, seq);
        fail += !r || r->error || decode_check(d, &j);
    }
    snapshot_valid = 0;
    salsads_log("rsx3d RGBA8 SPE: 14 casos, %u fallos", fail);
    return fail;
}
int rsx3d_decode_raster_test(const r3d_job *job)
{
    if (!decode_worker || decode_ppe) return 0;
    r3d_job j = *job; j.band = 16; j.nunits = 1; j.unit = 0; j.bt_ea = 0;
    uint32_t seq = spukit_post(decode_worker, JOB_RASTER3D, &j, sizeof j);
    const spukit_result *r = spukit_wait(decode_worker, seq);
    job_r3d_result result; memcpy(&result, r->data, sizeof result);
    return r->error || result.rc;
}
static int texture(const r3d_tex *t, rsxkit_texture *out, rsx3d_stats *st)
{
    uint64_t t0 = ticks();
    struct cache_entry *entry = NULL;
    for (uint32_t i = 0; i < nc; i++) {
        if (cache[i].key == t->key && cache[i].pal == t->pkey) {
            entry = &cache[i]; entry->wanted = 1;
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
    if (!entry) {
        entry = &cache[nc++]; memset(entry, 0, sizeof *entry);
        used = (used + pitch * h + 127u) & ~127u;
    }
    entry->wanted = 1;
    if (!entry->rows) memset(entry->pages, 0, sizeof entry->pages);
    decoding = entry;
    t0 = ticks();
    memset(out, 0, sizeof *out);
    out->offset = offset + dest; out->pitch = pitch;
    out->width = w; out->height = h; out->format = RSXKIT_RGBA8888;
    out->location = RSXKIT_MEM_MAIN; out->filter = RSXKIT_FILTER_NEAREST;
    out->xf[0] = 1.0f / w; out->xf[1] = 1.0f / h;
    if (decode_worker && !decode_ppe) {
        entry->key = t->key; entry->pal = t->pkey; entry->tx = *out; cache_source[entry-cache] = *t;
        decode_missing = 1; goto wrap;
    }
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
    const int incremental_issue = stat("/dev_hdd0/salsads/3d-rsx-envio-anterior", &flag) != 0;
    rsxkit_3d_incremental(incremental_issue);
    unbatched = stat("/dev_hdd0/salsads/3d-rsx-sin-lotes", &flag) == 0;
    decode_verify = stat("/dev_hdd0/salsads/3d-rsx-prueba", &flag) == 0;
    decode_ppe = stat("/dev_hdd0/salsads/3d-rsx-texturas-ppe", &flag) == 0;
    scalar_vertices = stat("/dev_hdd0/salsads/3d-rsx-vertices-ppe", &flag) == 0;
    old_prepare = stat("/dev_hdd0/salsads/3d-rsx-preparacion-anterior", &flag) == 0;
    full_identity = stat("/dev_hdd0/salsads/3d-rsx-identidad-completa", &flag) == 0;
    coarse_cache = stat("/dev_hdd0/salsads/3d-rsx-cache-gruesa", &flag) == 0;
    profile = stat("/dev_hdd0/salsads/3d-rsx-prueba", &flag) == 0 ||
              stat("/dev_hdd0/salsads/3d-rsx-perfil", &flag) == 0;
    fog_spe = stat("/dev_hdd0/salsads/3d-rsx-niebla-spe", &flag) == 0;
    shadow_unbatched = stat("/dev_hdd0/salsads/3d-rsx-sombra-sin-lotes", &flag) == 0;
    shadow_general = stat("/dev_hdd0/salsads/3d-rsx-sombra-general", &flag) == 0;
    shadow_spe = stat("/dev_hdd0/salsads/3d-rsx-sombra-spe", &flag) == 0;
    toon_spe = stat("/dev_hdd0/salsads/3d-rsx-toon-spe", &flag) == 0;
    salsads_log("rsx3d preparación: vértices %s, RGBA8 %s, envío %s; presupuesto %u texeles",
                scalar_vertices || old_prepare ? "escalar PPE" : "VMX PPE",
                decode_ppe ? "PPE" : "SPE por lotes", incremental_issue ? "incremental" : "anterior", RSX_DECODE_PIXELS);
    tick_hz = sysGetTimebaseFrequency();
    for (unsigned slot = 0; slot < 2; slot++)
        memset(mem + OUT_OFF(slot) + R3D_W * R3D_H * 4u, 31, 512);
    if (rsxkit_map_main(mem, ARENA, &offset)) return -1;
    target = rsxkit_target_create_rgba(R3D_W, R3D_H);
    depth = rsxkit_depth_create(R3D_W, R3D_H);
    for (unsigned slot = 0; slot < 2; slot++)
        output[slot] = rsxkit_surface_wrap_main(R3D_W, R3D_H, RSXKIT_POST_RGBA8, mem + OUT_OFF(slot), offset + OUT_OFF(slot), R3D_W * 4);
    shade_w = rsxkit_program_create(shade_w_fpo);
    shade = rsxkit_program_create(shade_fpo); pack = rsxkit_program_create(pack_fpo);
    fog = rsxkit_program_create(fog_fpo); fog_w = rsxkit_program_create(fog_w_fpo);
    shadow = rsxkit_program_create(shadow_fpo); shadow_w = rsxkit_program_create(shadow_w_fpo);
    mask = rsxkit_program_create(mask_fpo); mask_w = rsxkit_program_create(mask_w_fpo);
    toon = rsxkit_program_create(toon_fpo); toon_w = rsxkit_program_create(toon_w_fpo);
    if (!target || !depth || !output[0] || !output[1] || !shade || !shade_w || !pack || !toon || !toon_w || !shadow || !shadow_w || !mask || !mask_w || !fog || !fog_w) return -1;
    if (stat("/dev_hdd0/salsads/3d-rsx-2x", &flag) == 0) hd_scale=2;
    if (stat("/dev_hdd0/salsads/3d-rsx-3x", &flag) == 0) hd_scale=3;
    if (stat("/dev_hdd0/salsads/3d-rsx-4x", &flag) == 0) hd_scale=4;
    if (hd_scale > 1 && !decode_verify) {
        salsads_log("rsx3d escala solicitada %ux: presentación nativa 1x; HD limitado al reproductor; pendiente validar en consola",hd_scale);
        hd_scale = 1;
    }
    if (hd_scale > 1) {
        hd_refresh = stat("/dev_hdd0/salsads/3d-rsx-hd-sin-recarga", &flag) != 0;
        salsads_log("rsx3d HD recarga de metadatos: %s",hd_refresh ? "activa" : "desactivada");
        hd_mem=memalign(HD_STRIDE,4*HD_STRIDE);
        if (!hd_mem || rsxkit_map_main(hd_mem,4*HD_STRIDE,&hd_offset)) return -1;
        memset(hd_mem,0,4*HD_STRIDE);
        unsigned w=256*hd_scale,h=192*hd_scale;
        if (rsxkit_depth_reserve(w,h)) return -1;
        hd_depth=rsxkit_depth_create(w,h);
        hd_program=rsxkit_program_create(compose_hd_fpo);
        for (unsigned i=0;i<2;i++) {
            hd_target[i]=rsxkit_target_create_rgba(w,h);
            hd_saved[i]=rsxkit_target_create_rgba(w,h);
            if (!hd_target[i] || !hd_saved[i]) return -1;
        }
        if (!hd_depth || !hd_program) return -1;
    }
    salsads_log("rsx3d escala: %ux; captura nativa independiente; HD solo GDDR3",hd_scale);
    reason = "Z nativa; W por fragmento";
    ready = 1;
    if (!decode_ppe && (stat("/dev_hdd0/salsads/3d-rsx-prueba", &flag) == 0 ||
                       stat("/dev_hdd0/salsads/autotest", &flag) == 0)) {
        spukit_init(6);
        decode_worker = spukit_worker_start(spu_rsxprep_bin, "rsx-prep", 0);
        decode_owned = decode_worker != NULL;
        if (!decode_worker) return -1;
    }
    return 0;
}
void rsx3d_shutdown(void)
{
    rsx3d_decode_worker(NULL);
    rsxkit_finish();
    rsxkit_program_destroy(fog); rsxkit_program_destroy(fog_w);
    rsxkit_program_destroy(shadow); rsxkit_program_destroy(shadow_w);
    rsxkit_program_destroy(mask); rsxkit_program_destroy(mask_w);
    rsxkit_program_destroy(toon); rsxkit_program_destroy(toon_w);
    rsxkit_program_destroy(shade_w); rsxkit_program_destroy(shade); rsxkit_program_destroy(pack);
    rsxkit_surface_destroy(output[0]); rsxkit_surface_destroy(output[1]); rsxkit_target_destroy(target); rsxkit_depth_destroy(depth);
    for (unsigned i=0;i<2;i++) { rsxkit_target_destroy(hd_target[i]); rsxkit_target_destroy(hd_saved[i]); }
    rsxkit_depth_destroy(hd_depth); rsxkit_program_destroy(hd_program);
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
static int render_frame(const r3d_job *j, unsigned slot, rsx3d_stats *st)
{
    const uint64_t start = sysGetSystemTime();
    const uint64_t *versions = !full_identity && version_ea == j->vram_ea ? next_versions : NULL;
    next_versions = NULL;
    const gx_frame *f = (const gx_frame *)(uintptr_t)j->frame_ea;
    r3d_rstate rs;
    uint32_t ncmd = 0, nv = 0;
    memset(st, 0, sizeof *st);
    last_valid = 0;
    if (!ready || decode_error) { reason = "sin recursos o fallo de RGBA8"; return -1; }
    if (!decode_collect()) { reason = "texturas SPE pendientes"; return decode_error ? -1 : 0; }
    decode_count = 0; decode_missing = 0;
    for (unsigned k = 0; k < nc; k++) cache[k].wanted = 0;
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
    /* La caché es compartida: no invalidar ni sobrescribir texeles que el
     * trabajo anterior aún pueda leer. Normalmente acabó durante la emulación.
     * Su exportación sigue pendiente en la otra ranura. */
    uint64_t reuse_start = sysGetSystemTime();
    if (texture_fence) rsxkit_fence_wait(texture_fence);
    st->reuse_us = sysGetSystemTime() - reuse_start;
    if (rsxkit_lost()) return -1;
    if (modes & ((1u << RSX_TOON) | (1u << RSX_HIGHLIGHT))) {
        for (unsigned i = 0; i < 32; i++) {
            uint8_t *b = mem + TOON_OFF(slot) + 4*i;
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
    } else if (versions || memcmp(mem + SNAP_OFF, (const void *)(uintptr_t)j->vram_ea, R3D_VRAM_ZERO_OFF)) {
        uint32_t changed[PAGE_WORDS] = {0};
        for (uint32_t a = 0; a < R3D_VRAM_ZERO_OFF; a += PAGE_SIZE) {
            if (versions && versions_valid && versions[a >> 14] == old_versions[a >> 14]) {
                a += 0x4000u - PAGE_SIZE; continue;
            }
            const uint8_t *src = (const uint8_t *)(uintptr_t)j->vram_ea + a;
            if (memcmp(mem + SNAP_OFF + a, src, PAGE_SIZE)) {
                changed[a / PAGE_SIZE / 32u] |= 1u << (a / PAGE_SIZE % 32u);
                memcpy(mem + SNAP_OFF + a, src, PAGE_SIZE);
            }
        }
        for (uint32_t i = 0; i < nc; i++) for (unsigned w = 0; w < PAGE_WORDS; w++) {
            if (coarse_cache ? changed[w] != 0 : (changed[w] & cache[i].pages[w]) != 0) {
                if (coarse_cache) { used = nc = 0; break; }
                cache[i].valid = 0; cache[i].rows = 0; break;
            }
        }
    }
    versions_valid = versions != NULL;
    if (versions) memcpy(old_versions, versions, sizeof old_versions);
    st->identity_us = ticks() - t0;
    cur = j;
    rsx3d_vertex *v = (rsx3d_vertex *)(void *)(mem + VERT_OFF(slot));
    for (uint32_t i = 0; i < f->npolys; i++) {
        t0 = ticks();
        const gx_poly *gp = &f->poly[order[i]];
        const gx_vertex *vv[GX_POLY_MAXV]; r3d_poly p;
        if (old_prepare) memset(&p, 0, sizeof p);
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
            d->tex[1].offset = offset + TOON_OFF(slot); d->tex[1].pitch = 128;
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
        st->state_us += ticks() - t0;
        /* Sin textura: una textura blanca de 8x8 propia, colocada fuera de la arena. */
        if (p.textured) {
            if (!texture(&p.tex, &d->tex[0], st)) { reason = "arena de texturas llena"; used = nc = 0; return 0; }
        } else {
            if (old_prepare) memset(mem + OUT_OFF(slot) + R3D_W * R3D_H * 4u, 31, 512);
            d->tex[0].offset = offset + OUT_OFF(slot) + R3D_W * R3D_H * 4u;
            d->tex[0].pitch = 64; d->tex[0].width = d->tex[0].height = 8;
            d->tex[0].location = RSXKIT_MEM_MAIN; d->tex[0].format = RSXKIT_RGBA8888;
            d->tex[0].filter = RSXKIT_FILTER_NEAREST;
        }
        if (decode_missing) continue;
        t0 = ticks();
        rsx3d_vertex pv[GX_POLY_MAXV];
        int count = old_prepare ? (p.n - 2) * 3 : p.n;
        for (int a = 0; a < count; a++) {
            int ix = old_prepare ? (a % 3 ? a / 3 + a % 3 : 0) : a;
            const gx_vertex *g = vv[ix]; rsx3d_vertex *b = old_prepare ? &v[nv++] : &pv[ix];
            if (scalar_vertices || old_prepare) rsx3d_vertex_scalar(b, g, p.v[ix].w);
            else rsx3d_vertex_vector(b, g, p.v[ix].w);
        }
        if (!old_prepare) for (int k = 1; k + 1 < p.n; k++) {
            v[nv++] = pv[0]; v[nv++] = pv[k]; v[nv++] = pv[k+1];
        }
        st->vertex_us += ticks() - t0;
    }
    if (decode_missing) {
        decode_plan(); cold_frames++;
        reason = "texturas SPE pendientes";
        st->prepare_us = sysGetSystemTime() - start - st->reuse_us;
        return 0;
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
    st->state_us = st->state_us * 1000000u / tick_hz;
    st->upload_us = st->upload_us * 1000000u / tick_hz;
    st->prepare_us = sysGetSystemTime() - start - st->reuse_us;
    /* Terminar con raster y exportación nativos conserva el estado de salida
     * de la ruta 1x, también con dos trabajos pendientes. */
    for (unsigned render_pass=0;render_pass<(hd_scale>1?2u:1u);render_pass++) {
    const unsigned hd_pass = hd_scale>1 && render_pass==0;
    if (rsxkit_bind(hd_pass ? hd_target[slot] : target, hd_pass ? hd_depth : depth, R3D_W, R3D_H)) return -1;
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
    rsxkit_stamp(hd_pass ? 48 + slot*2 : 56 + slot * 3);
    rsxkit_clear(RSXKIT_CLEAR_COLOR | RSXKIT_CLEAR_DEPTH | RSXKIT_CLEAR_ALPHA, encoded, rs.clear_depth / 16777215.0f, NULL);
    rsxkit_clear_stencil(shadows ? ((ca ? 128 : 0) | oid[AT_OID(rs.clear_attr)]) : (ca ? 64 : 255));
    rsxkit_vertices(offset + VERT_OFF(slot), RSXKIT_MEM_MAIN);
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
    if (hd_pass) { rsxkit_stamp(49+slot*2); hd_native[slot]=j->out_ea; continue; }
    rsxkit_stamp(57 + slot * 3);
    rsxkit_texture tx; rsxkit_target_texture(target, 0, R3D_H, &tx);
    rsxkit_post_pass pass; memset(&pass, 0, sizeof pass);
    pass.program = pack; pass.out = output[slot]; pass.inputs = 1;
    pass.in[0] = rsxkit_post_texture_input(&tx, RSXKIT_FILTER_NEAREST, RSXKIT_POST_CLAMP);
    if (rsxkit_post_run(&pass)) return -1;
    rsxkit_stamp(58 + slot * 3);
    }
    flight[slot].fence = texture_fence = rsxkit_fence();
    flight[slot].out_ea = j->out_ea;
    flight[slot].pending = 1;
    st->total_us = sysGetSystemTime() - start;
    flight[slot].stats = *st;
    reason = rs.wbuf ? "W por fragmento; interpolación triangular" : "Z nativa; cobertura e interpolación triangular";
    return 1;
}

int rsx3d_submit(const r3d_job *j, unsigned slot, rsx3d_stats *st)
{
    static unsigned frames, rendered, counts[RSX_MODE_COUNT];
    if (slot > 1 || flight[slot].pending) return -1;
    uint64_t start = sysGetSystemTime();
    hd_native[slot] = 0;
    rsx3d_hd_invalidate(j->out_ea);
    int result = render_frame(j, slot, st);
    if (result <= 0) {
        st->total_us = sysGetSystemTime() - start;
        st->prepare_us = st->total_us - st->reuse_us;
    }
    /* Un error tras emitir órdenes también debe proteger la caché. */
    if (result < 0) { texture_fence = rsxkit_fence(); rsxkit_fence_wait(texture_fence); }
    const gx_frame *f = (const gx_frame *)(uintptr_t)j->frame_ea;
    uint32_t modes = rsx_modes(f, j->flags);
    rendered += result > 0;
    for (unsigned k = 0; k < RSX_MODE_COUNT; k++) counts[k] += (modes >> k) & 1u;
    if (++frames == 120) {
        salsads_log("DATO rsx3d censo: cuadros %u modo RSX %u SPE %u; imagen-borrado %u niebla %u bordes %u toon %u resaltado %u mascara-sombra %u sombra %u igualdad %u alambre %u",
                    frames, rendered, frames-rendered, counts[0], counts[1], counts[2], counts[3],
                    counts[4], counts[5], counts[6], counts[7], counts[8]);
        salsads_log("DATO rsx3d frio: cuadros pendientes %u ocupado %u lotes %u; presupuesto %u texeles, espera PPE 0; SPE %.3f ms, texeles %llu",
                    cold_frames, busy_frames, decode_batches, RSX_DECODE_PIXELS,
                    decode_ticks * 1000.0 / spukit_tick_hz(), (unsigned long long)decode_pixels);
        cold_frames = busy_frames = decode_batches = 0; decode_ticks = decode_pixels = 0;
        frames = rendered = 0; memset(counts, 0, sizeof counts);
    }
    return result;
}

int rsx3d_wait(unsigned slot, rsx3d_stats *st)
{
    if (slot > 1 || !flight[slot].pending) return -1;
    *st = flight[slot].stats;
    uint64_t t = sysGetSystemTime();
    rsxkit_fence_wait(flight[slot].fence);
    st->wait_us = sysGetSystemTime() - t;
    if (rsxkit_lost()) return -1;
    t = sysGetSystemTime();
    memcpy((void *)(uintptr_t)flight[slot].out_ea, mem + OUT_OFF(slot), R3D_W * R3D_H * 4u);
    st->copy_us = sysGetSystemTime() - t;
    st->total_us += st->wait_us + st->copy_us;
    st->raster_ns = rsxkit_stamp_ns(57 + slot * 3) - rsxkit_stamp_ns(56 + slot * 3);
    st->export_ns = rsxkit_stamp_ns(58 + slot * 3) - rsxkit_stamp_ns(57 + slot * 3);
    st->hd_raster_ns = hd_scale > 1 ? rsxkit_stamp_ns(49+slot*2)-rsxkit_stamp_ns(48+slot*2) : 0;
    if (hd_scale>1) {
        static unsigned samples;
        hd_raster_ns += st->hd_raster_ns;
        if (++samples==120) {
            salsads_log("DATO rsx3d HD raster: escala %ux; adicional %.3f ms por cuadro",hd_scale,hd_raster_ns/120000000.0);
            samples=0; hd_raster_ns=0;
        }
    }
    flight[slot].pending = 0;
    return 1;
}

int rsx3d_render(const r3d_job *j, rsx3d_stats *st)
{
    int result = rsx3d_submit(j, 0, st);
    /* Solo la API síncrona del diagnóstico precalienta. En juego submit nunca
     * espera RGBA8: kick se hace después de publicar el cuadro de respaldo. */
    for (unsigned retry = 0; !result && decode_worker &&
         !strcmp(reason, "texturas SPE pendientes") && retry < 512; retry++) {
        rsx3d_decode_kick();
        if (decode_seq) spukit_wait(decode_worker, decode_seq);
        result = rsx3d_submit(j, 0, st);
    }
    return result > 0 ? rsx3d_wait(0, st) : result;
}

/* Solo autopruebas: cambiar de destino y emitir un borrado antes de leer
 * evita reutilizar la primera lectura de la superficie en RPCS3. No toca
 * las exportaciones nativas ni la presentación del juego. */
static void hd_test_finish(void)
{
    rsxkit_bind(target,depth,256,192);
    rsxkit_clear(RSXKIT_CLEAR_COLOR,0,1,NULL);
    rsxkit_fence_wait(rsxkit_fence());
}

/* Solo diagnóstico: salida nativa del compositor, nunca lectura de un plano HD. */
int rsx3d_hd_contract_test(uint64_t ea, uint32_t sample)
{
    if (hd_scale == 1) return 0;
    if (!rsx3d_hd_acquire(ea,0)) return 1;
    g2d_hd_pixel *p=(g2d_hd_pixel *)(void *)hd_mem;
    for (unsigned i=0;i<256*192;i++) {
        uint8_t *b=p[i].b; memset(b,0,16);
        b[0]=i&31; b[1]=(i>>5)&31; b[2]=(i>>10)&31;
        b[3]=1+i%4; b[4]=(i*7)&31; b[5]=(i*11)&31; b[6]=(i*13)&31;
        b[7]=i%17; b[8]=(i/17)%17;
        b[9]=(i/289)%3*32+(i/867)%17;
        b[10]=(i/14739)%3*32+(i/31)%17;
        b[11]=(i/7)&255; b[12]=(i/1792)&1;
    }
    rsxkit_post_pass pass; hd_compose_pass(&pass,0,0,0,256,192);
    pass.out=output[1];
    if (rsxkit_post_run(&pass)) return 1;
    hd_test_finish();
    unsigned bad=0;
    for (unsigned i=0;i<256*192;i++) {
        unsigned sx=(i%256+p[i].b[11]+256u*p[i].b[12])&511;
        uint16_t expected=g2d_hd_eval(&p[i],sx<256?sample:0);
        const uint8_t *b=mem+OUT_OFF(1)+i*4;
        uint16_t actual=((b[1]*31u+127)/255) | (((b[2]*31u+127)/255)<<5) | (((b[3]*31u+127)/255)<<10);
        if (expected!=actual && bad++<2)
            salsads_log("rsx3d HD diferencia píxel %u: esperado %04x recibido %04x bytes %02x %02x %02x %02x operación %u",i,expected,actual,b[0],b[1],b[2],b[3],p[i].b[3]);
    }
    salsads_log("rsx3d HD composición %ux muestra %08x: 49152 casos, %u fallos",hd_scale,sample,bad);
    return bad!=0;
}

/* Línea cambiante: entradas y salida se contrastan por canal, sin SPE. */
int rsx3d_hd_line_test(uint64_t ea, uint32_t sample)
{
    if (hd_scale == 1) return 0;
    unsigned bad=0;
    rsxkit_program *probe=rsxkit_program_create(probe_hd_fpo);
    if (!probe) return 1;
    for (unsigned round=0;round<4;round++) {
        unsigned set=round==2;
        if (!rsx3d_hd_acquire(ea,set)) { bad++; goto done; }
        g2d_hd_pixel *p=(g2d_hd_pixel *)(void *)(hd_mem+set*HD_STRIDE);
        for (unsigned x=0;x<256;x++) {
            uint8_t *b=p[x].b; memset(b,0,16);
            b[0]=(x+3+round*7)&31; b[1]=(x*3+11+round*5)&31; b[2]=(x*7+19+round*3)&31;
            b[3]=x%5; b[4]=(x+23+round)&31; b[5]=(x*5+17+round)&31; b[6]=(x*3+7+round)&31;
            b[7]=9; b[8]=7; b[9]=(round%3)*32+5; b[10]=0;
        }
        rsxkit_post_pass pass; hd_compose_pass(&pass,set,0,0,256,192);
        pass.out=output[1]; pass.rect.h=1;
        for (unsigned input=0;input<5;input++) {
            pass.program=probe; pass.k[0][1]=input;
            if (rsxkit_post_run(&pass)) { bad++; goto done; }
            hd_test_finish();
            const uint8_t *raw=mem+OUT_OFF(1);
            uint8_t expected[4];
            if (input<4) memcpy(expected,p[0].b+input*4,4);
            else {
                expected[0]=((((sample>>24)&31)+1)*255+16)/32;
                expected[1]=(sample&63)+16; expected[2]=((sample>>8)&63)+16;
                expected[3]=((sample>>16)&63)+16;
            }
            bad += memcmp(raw,expected,4)!=0;
            salsads_log("rsx3d HD sonda %u/%u: ARGB esperado %u %u %u %u recibido %u %u %u %u",round,input,expected[0],expected[1],expected[2],expected[3],raw[0],raw[1],raw[2],raw[3]);
        }
        pass.program=hd_program; pass.k[0][1]=0;
        if (rsxkit_post_run(&pass)) { bad++; goto done; }
        hd_test_finish();
        for (unsigned x=0;x<256;x++) {
            uint16_t c=g2d_hd_eval(&p[x],sample);
            const uint8_t *b=mem+OUT_OFF(1)+4*x;
            unsigned r=(b[1]*31u+127)/255,g=(b[2]*31u+127)/255,bl=(b[3]*31u+127)/255;
            bad += r!=(c&31) || g!=((c>>5)&31) || bl!=((c>>10)&31) || b[0]!=255;
            if (x<5) salsads_log("rsx3d HD línea %u/%u: RGB esperado %u %u %u recibido %u %u %u A %u",round,x,c&31,(c>>5)&31,(c>>10)&31,r,g,bl,b[0]);
        }
    }
done:
    rsxkit_program_destroy(probe);
    salsads_log("rsx3d HD línea cambiante: 1024 píxeles y 20 sondas; ranuras 0,0,1,0, %u fallos",bad);
    return bad!=0;
}

int rsx3d_hd_native_test(const uint16_t *native)
{
    rsxkit_post_pass pass; hd_compose_pass(&pass,0,0,0,256,192);
    pass.out=output[1];
    const uint8_t *src=hd_mem, *pub=hd_mem+2*HD_STRIDE;
    salsads_log("rsx3d HD entrada: %02x %02x %02x %02x / %02x %02x %02x %02x; publicada %02x %02x %02x %02x",src[0],src[1],src[2],src[3],src[4],src[5],src[6],src[7],pub[0],pub[1],pub[2],pub[3]);
    if (rsxkit_post_run(&pass)) return -1;
    hd_test_finish();
    unsigned bad=0;
    for (unsigned i=0;i<256*192;i++) {
        const uint8_t *b=mem+OUT_OFF(1)+i*4;
        uint16_t actual=(((b[1]*31u+127)/255)<<11) | (((b[2]*31u+127)/255)<<6) | (((b[3]*31u+127)/255)<<1) | 1;
        if (native[i]!=actual && bad++<2)
            salsads_log("rsx3d HD SPE diferencia %u: nativo %04x shader %04x bytes %02x %02x %02x %02x",i,native[i],actual,b[0],b[1],b[2],b[3]);
    }
    return (int)bad;
}

/* Banco aislado: imagen 3D grabada y mezcla 2D sintética prearmada.
 * No incluye emulación, producción SPE de metadatos ni espera de flip. */
int rsx3d_hd_bench(uint64_t ea, const char *name, const rsx3d_stats *st)
{
    if (hd_scale == 1) return 0;
    unsigned w=256*hd_scale, h=192*hd_scale;
    rsxkit_surface *dst=rsxkit_surface_create(w,h,RSXKIT_POST_RGBA8);
    if (!dst) return 1;
    uint64_t copy=0, compose=0, publish=0;
    unsigned bad=0;
    for (unsigned round=0;round<10;round++) {
        unsigned set=round&1;
        uint64_t meta=rsx3d_hd_acquire(ea,set);
        if (!meta) { bad=1; break; }
        if (round<2) {
            g2d_hd_pixel *p=(g2d_hd_pixel *)(uintptr_t)meta;
            for (unsigned i=0;i<256*192;i++) {
                uint8_t *b=p[i].b; memset(b,0,16);
                b[0]=i&31; b[1]=(i>>5)&31; b[2]=(i>>10)&31;
                b[3]=i%5; b[4]=17; b[5]=9; b[6]=23;
                b[7]=9; b[8]=7; b[9]=37;
            }
        }
        rsxkit_post_pass pass;
        uint64_t cpu=hd_compose_pass(&pass,set,0,0,w,h);
        pass.out=dst;
        rsxkit_stamp(62);
        if (rsxkit_post_run(&pass)) { bad=1; break; }
        rsxkit_stamp(63);
        rsxkit_fence_wait(rsxkit_fence());
        if (round>=2) {
            copy += rsxkit_stamp_ns(53+set*2)-rsxkit_stamp_ns(52+set*2);
            compose += rsxkit_stamp_ns(63)-rsxkit_stamp_ns(62);
            publish += cpu;
        }
    }
    if (!bad) salsads_log("DATO rsx3d HD banco %s: %ux %ux%u; 8 muestras; raster nativo %.3f HD %.3f exportación %.3f copia local %.3f composición %.3f total GPU %.3f ms; publicación PPE %.3f ms; 2D sintético, sin SPE/flip",
        name,hd_scale,w,h,st->raster_ns/1e6,st->hd_raster_ns/1e6,st->export_ns/1e6,
        copy/8e6,compose/8e6,(st->raster_ns+st->hd_raster_ns+st->export_ns)/1e6+(copy+compose)/8e6,publish/8000.0);
    rsxkit_surface_destroy(dst);
    return bad;
}
