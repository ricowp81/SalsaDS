/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Se incluye tras las rutinas de capas: mismas prioridades y ventanas,
 * sin consultar ni modificar la muestra nativa de BG0. */
#include "gpu2d/gpu2d_hd.h"
static unsigned hd_effect(unsigned mode, unsigned factor)
{
    return (mode == 1 || mode == 2) ? mode * 32 + g2d_ev(factor & 31) : 0;
}
static uint16_t hd_layer_color(const gpu2d_ctx *c, unsigned s)
{
    return s & 0x8000 ? s & 0x7fff : c->pal[s];
}
static void hd_pair(const gpu2d_ctx *c, const g2d_job *j, unsigned p,
                    const int *ent, int nent, unsigned x, int present,
                    unsigned *colors, unsigned *ids, unsigned *meta)
{
    unsigned v = x / 8, lane = x % 8, found = 0;
    int win = (j->dc & (G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN)) != 0;
    colors[0] = colors[1] = c->pal[0]; ids[0] = ids[1] = 5; *meta = 0;
    for (int k = nent - 1; k >= 0 && found < 2; k--) {
        unsigned e = ent[k], lid = e < 4 ? e : 4, m = 0;
        unsigned s = e < 4 ? g2d_v8_lane(c->bg[e][8+v],lane) : g2d_v8_lane(c->oidx[p][8+v],lane);
        if (win && !g2d_v8_lane(c->allow[lid][v],lane)) continue;
        if (!e && j->bg_type[0] == BG_3D) {
            if (!present) continue;
            s = 0x8000; lid = 7;
        }
        if (s == G2D_TRANSP) continue;
        if (e >= 4) {
            m = g2d_v8_lane(c->omet[p][8+v],lane);
            if ((m & 7) != e - 4) continue;
            if (m & 24) lid = 6;
        }
        colors[found] = hd_layer_color(c, s); ids[found] = lid;
        if (!found) *meta = m;
        found++;
    }
}
static void hd_operation(const g2d_job *j, const unsigned *col, const unsigned *id,
                         unsigned meta, int fx, g2d_hd_pixel *out)
{
    uint8_t *b = out->b;
    unsigned ctrl = j->regs[G2D_R_BLDCNT], mode = (ctrl >> 6) & 3;
    unsigned top = id[0] == 7 ? 0 : id[0] == 6 ? 4 : id[0];
    unsigned sec = id[1] == 7 ? 0 : id[1] == 6 ? 4 : id[1];
    unsigned second = (ctrl >> (8 + sec)) & 1, first = (ctrl >> top) & 1;
    unsigned alpha = j->regs[G2D_R_BLDALPHA];
    unsigned a = g2d_ev(alpha & 31), d = g2d_ev((alpha >> 8) & 31);
    unsigned bright = fx && first && mode >= 2 ? hd_effect(mode - 1, j->regs[G2D_R_BLDY]) : 0;
    unsigned op = id[0] == 7 ? G2D_HD_SAMPLE : G2D_HD_CONSTANT;
    uint16_t c = col[0];
    if (id[0] == 7 && second) {
        op = G2D_HD_ALPHA; c = col[1]; bright = 0;
    } else if (second && (id[0] == 6 || (fx && first && mode == 1))) {
        if (id[0] == 6 && (meta & 16)) { a = ((meta >> 5) & 15) + 1; d = 16 - a; }
        if (id[1] == 7) op = G2D_HD_BLEND;
        else c = g2d_blend(c, col[1], a, d);
        bright = 0;
    }
    g2d_hd_color(b + 4, c); b[3] = op; b[7] = a; b[8] = d; b[9] = bright;
    b[10] = hd_effect(j->regs[G2D_R_MASTER_BRIGHT] >> 14, j->regs[G2D_R_MASTER_BRIGHT]);
}
/* Un bloque con las mismas capas y ventanas tiene los mismos metadatos:
 * BG0 3D no se compara porque el contrato conserva ambas alternativas. */
static int hd_uniform(g2d_v8 a)
{
    return !g2d_v8_any(g2d_v8_andc(g2d_v8_splat(0xffff),
        g2d_v8_eq(a, g2d_v8_splat(g2d_v8_lane(a, 0)))));
}
static int hd_repeat(const gpu2d_ctx *c, const g2d_job *j, unsigned p,
                     const int *ent, int nent, unsigned v, int win)
{
    if (win) for (unsigned k = 0; k < 6; k++)
        if (!hd_uniform(c->allow[k][v])) return 0;
    for (int k = 0; k < nent; k++) {
        unsigned e = ent[k];
        if (!e && j->bg_type[0] == BG_3D) continue;
        if (!hd_uniform(e < 4 ? c->bg[e][8+v] : c->oidx[p][8+v])) return 0;
        if (e >= 4 && !hd_uniform(c->omet[p][8+v])) return 0;
    }
    return 1;
}
static void hd_export(gpu2d_ctx *c, const g2d_job *j, unsigned p,
                       const int *ent, int nent, uint64_t ea, int scalar)
{
    /* draw_3d ya consumió la línea desplazada: reutilizarla sin crecer LS.
     * Esperar cada trozo antes de volver a escribirlo. */
    g2d_hd_pixel *buf = (g2d_hd_pixel *)(void *)c->l3d_sh;
    int win = (j->dc & (G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN)) != 0;
    for (unsigned x0 = 0; x0 < 256; x0 += 64) {
        for (unsigned i = 0; i < 64;) {
            unsigned repeat = !scalar && !(i & 7) && hd_repeat(c,j,p,ent,nent,(x0+i)/8,win) ? 8 : 1;
            unsigned x = x0+i, col[2], id[2], meta;
            int fx = !win || g2d_v8_lane(c->allow[5][x/8],x%8);
            g2d_hd_pixel *q = &buf[i]; memset(q, 0, sizeof *q);
            hd_pair(c, j, p, ent, nent, x, 0, col, id, &meta);
            hd_operation(j, col, id, meta, fx, q);
            uint16_t absent = g2d_hd_bright(g2d_hd_bright(g2d_hd_rgb(q->b+4), q->b[9]), q->b[10]);
            hd_pair(c, j, p, ent, nent, x, 1, col, id, &meta);
            hd_operation(j, col, id, meta, fx, q);
            g2d_hd_color(q->b, absent);
            unsigned hofs = j->regs[G2D_R_BGHOFS] & 511;
            q->b[11] = hofs; q->b[12] = hofs >> 8;
            for (unsigned k = 1; k < repeat; k++) memcpy(&buf[i+k], q, sizeof *q);
            i += repeat;
        }
        g2d_put(buf, ea + x0 * sizeof *buf, 64 * sizeof *buf, G2D_TAG_OUT + p);
        g2d_wait(1u << (G2D_TAG_OUT + p));
    }
}
