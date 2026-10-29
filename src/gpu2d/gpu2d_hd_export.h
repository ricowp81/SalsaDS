/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Se incluye tras las rutinas de capas: mismas prioridades y ventanas,
 * sin consultar ni modificar la muestra nativa de BG0. La ruta vectorial es
 * la normal; la escalar por píxel queda como referencia (G2D_F_HD_SCALAR). */
#include "gpu2d/gpu2d_hd.h"
static inline unsigned hd_effect(unsigned mode, unsigned factor)
{
    return (mode == 1 || mode == 2) ? mode * 32 + g2d_ev(factor & 31) : 0;
}
#ifndef __SPU__
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
#endif
/* Versión vectorial: ocho píxeles por vector con la misma selección de las dos
 * capas visibles y la misma operación que hd_pair/hd_operation; sin bloques
 * repetidos. Los campos se transponen a los 16 bytes de cada píxel. */
static __attribute__((noinline)) void hd_pair_vec(const gpu2d_ctx *c, const g2d_job *j, unsigned p,
                        const int *ent, int nent, unsigned v, int present, int win,
                        g2d_v8 *col, g2d_v8 *id, g2d_v8 *meta)
{
    const g2d_v8 ones = g2d_v8_splat(0xffff), zero = g2d_v8_splat(0), tr = g2d_v8_splat(G2D_TRANSP);
    const g2d_v8 four = g2d_v8_splat(4), six = g2d_v8_splat(6), seven = g2d_v8_splat(7);
    g2d_v8 have0 = zero, have1 = zero, idx0 = zero, idx1 = zero, id0 = g2d_v8_splat(5), id1 = g2d_v8_splat(5), m0 = zero;
    for (int k = nent - 1; k >= 0; k--) {
        unsigned e = ent[k];
        g2d_v8 s, lid, m, mv = zero;
        if (e < 4) {
            s = c->bg[e][8+v]; lid = g2d_v8_splat((uint16_t)e);
            m = g2d_v8_andc(ones, g2d_v8_eq(s, tr));
            if (win) m = g2d_v8_and(m, v8_nz(c->allow[e][v]));
            if (!e && j->bg_type[0] == BG_3D) {
                if (!present) continue;
                s = g2d_v8_splat(0x8000); lid = seven;
                m = win ? v8_nz(c->allow[0][v]) : ones;
            }
        } else {
            const g2d_v8 om = c->omet[p][8+v];
            s = c->oidx[p][8+v];
            m = g2d_v8_andc(ones, g2d_v8_eq(s, tr));
            m = g2d_v8_and(m, g2d_v8_eq(g2d_v8_and(om, seven), g2d_v8_splat((uint16_t)(e - 4))));
            if (win) m = g2d_v8_and(m, v8_nz(c->allow[4][v]));
            lid = g2d_v8_sel(four, six, v8_nz(g2d_v8_and(om, g2d_v8_splat(24))));
            mv = om;
        }
        {
            const g2d_v8 new0 = g2d_v8_andc(m, have0), new1 = g2d_v8_and(m, g2d_v8_andc(have0, have1));
            idx0 = g2d_v8_sel(idx0, s, new0); id0 = g2d_v8_sel(id0, lid, new0); m0 = g2d_v8_sel(m0, mv, new0);
            idx1 = g2d_v8_sel(idx1, s, new1); id1 = g2d_v8_sel(id1, lid, new1);
            have1 = g2d_v8_or(have1, new1); have0 = g2d_v8_or(have0, new0);
        }
    }
    col[0] = colors_of(c, idx0); col[1] = colors_of(c, idx1);
    id[0] = id0; id[1] = id1; *meta = m0;
}
/* Campos por carril: 0 op, 1 color (BGR555), 2 EVA, 3 EVB, 4 brillo. */
static __attribute__((noinline)) void hd_operation_vec(const g2d_job *j, const g2d_v8 *col, const g2d_v8 *id,
                             g2d_v8 meta, g2d_v8 fx, g2d_v8 *out)
{
    const unsigned ctrl = j->regs[G2D_R_BLDCNT], mode = (ctrl >> 6) & 3;
    const unsigned alpha = j->regs[G2D_R_BLDALPHA];
    const unsigned a = g2d_ev(alpha & 31), d = g2d_ev((alpha >> 8) & 31);
    const unsigned bright = mode >= 2 ? hd_effect(mode - 1, j->regs[G2D_R_BLDY]) : 0;
    const g2d_v8 ones = g2d_v8_splat(0xffff), zero = g2d_v8_splat(0), one = g2d_v8_splat(1);
    const g2d_v8 four = g2d_v8_splat(4), six = g2d_v8_splat(6), seven = g2d_v8_splat(7);
    const g2d_v8 is7 = g2d_v8_eq(id[0], seven), is6 = g2d_v8_eq(id[0], six), sec7 = g2d_v8_eq(id[1], seven);
    g2d_v8 top = g2d_v8_sel(g2d_v8_sel(id[0], zero, is7), four, is6);
    g2d_v8 sec = g2d_v8_sel(g2d_v8_sel(id[1], zero, sec7), four, g2d_v8_eq(id[1], six));
    const g2d_v8 first = v8_nz(g2d_v8_and(g2d_v8_shrv(g2d_v8_splat((uint16_t)ctrl), top), one));
    const g2d_v8 second = v8_nz(g2d_v8_and(g2d_v8_shrv(g2d_v8_splat((uint16_t)(ctrl >> 8)), sec), one));
    const g2d_v8 fxfirst = g2d_v8_and(fx, first);
    const g2d_v8 condA = g2d_v8_and(is7, second);
    const g2d_v8 condB = g2d_v8_and(g2d_v8_andc(second, is7),
                                    g2d_v8_or(is6, mode == 1 ? fxfirst : zero));
    const g2d_v8 metaA = g2d_v8_and(condB, g2d_v8_and(is6, v8_nz(g2d_v8_and(meta, g2d_v8_splat(16)))));
    const g2d_v8 av = g2d_v8_sel(g2d_v8_splat((uint16_t)a), g2d_v8_add(g2d_v8_and(g2d_v8_shr(meta, 5), g2d_v8_splat(15)), one), metaA);
    const g2d_v8 dv = g2d_v8_sel(g2d_v8_splat((uint16_t)d), g2d_v8_sub(g2d_v8_splat(16), av), metaA);
    g2d_v8 op = g2d_v8_sel(g2d_v8_splat(G2D_HD_CONSTANT), g2d_v8_splat(G2D_HD_SAMPLE), is7), cv = col[0];
    g2d_v8 br = g2d_v8_sel(zero, g2d_v8_splat((uint16_t)bright), mode >= 2 ? fxfirst : zero);
    op = g2d_v8_sel(op, g2d_v8_splat(G2D_HD_BLEND), g2d_v8_and(condB, sec7));
    cv = g2d_v8_sel(cv, v8_blend(col[0], col[1], av, dv), g2d_v8_andc(condB, sec7));
    op = g2d_v8_sel(op, g2d_v8_splat(G2D_HD_ALPHA), condA);
    cv = g2d_v8_sel(cv, col[1], condA);
    br = g2d_v8_sel(br, zero, g2d_v8_or(condA, condB));
    (void)ones;
    out[0] = op; out[1] = cv; out[2] = av; out[3] = dv; out[4] = br;
}
static inline g2d_v8 hd_bright_vec(g2d_v8 c, unsigned code)
{
    const g2d_v8 f = g2d_v8_splat((uint16_t)(code & 31));
    return code >= 64 ? v8_bright_down(c, f) : code >= 32 ? v8_bright_up(c, f) : c;
}
/* Dieciséis campos de un byte por carril (los tres últimos en cero) a ocho píxeles de 16 bytes. */
static __attribute__((noinline)) void hd_store_vec(g2d_hd_pixel *q, const g2d_v8 *f)
{
#ifdef __SPU__
    const vec_uchar16 lo = { 1, 17, 3, 19, 5, 21, 7, 23, 9, 25, 11, 27, 13, 29, 15, 31 };
    const vec_uchar16 hl = { 0, 1, 16, 17, 2, 3, 18, 19, 4, 5, 20, 21, 6, 7, 22, 23 };
    const vec_uchar16 hh = { 8, 9, 24, 25, 10, 11, 26, 27, 12, 13, 28, 29, 14, 15, 30, 31 };
    const vec_uchar16 wl = { 0, 1, 2, 3, 16, 17, 18, 19, 4, 5, 6, 7, 20, 21, 22, 23 };
    const vec_uchar16 wh = { 8, 9, 10, 11, 24, 25, 26, 27, 12, 13, 14, 15, 28, 29, 30, 31 };
    const vec_uchar16 dl = { 0, 1, 2, 3, 4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23 };
    const vec_uchar16 dh = { 8, 9, 10, 11, 12, 13, 14, 15, 24, 25, 26, 27, 28, 29, 30, 31 };
    vec_uchar16 pr[8], qd[8];
    for (unsigned k = 0; k < 8; k++) pr[k] = spu_shuffle((vec_uchar16)f[2*k], (vec_uchar16)f[2*k+1], lo);
    for (unsigned k = 0; k < 4; k++) {
        qd[2*k] = spu_shuffle(pr[2*k], pr[2*k+1], hl);
        qd[2*k+1] = spu_shuffle(pr[2*k], pr[2*k+1], hh);
    }
    vec_uchar16 *o = (vec_uchar16 *)(void *)q;
    for (unsigned h = 0; h < 2; h++) {
        const vec_uchar16 r0 = spu_shuffle(qd[h], qd[2+h], wl), r1 = spu_shuffle(qd[h], qd[2+h], wh);
        const vec_uchar16 s0 = spu_shuffle(qd[4+h], qd[6+h], wl), s1 = spu_shuffle(qd[4+h], qd[6+h], wh);
        o[4*h] = spu_shuffle(r0, s0, dl); o[4*h+1] = spu_shuffle(r0, s0, dh);
        o[4*h+2] = spu_shuffle(r1, s1, dl); o[4*h+3] = spu_shuffle(r1, s1, dh);
    }
#else
    for (unsigned i = 0; i < 8; i++)
        for (unsigned k = 0; k < 16; k++) q[i].b[k] = (uint8_t)g2d_v8_lane(f[k], (int)i);
#endif
}
static __attribute__((noinline)) void hd_export_vec(gpu2d_ctx *c, const g2d_job *j, unsigned p,
                          const int *ent, int nent, uint64_t ea)
{
    g2d_hd_pixel *buf = (g2d_hd_pixel *)(void *)c->l3d_sh; /* dos mitades de 32 píxeles */
    const int win = (j->dc & (G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN)) != 0;
    const unsigned master = hd_effect(j->regs[G2D_R_MASTER_BRIGHT] >> 14, j->regs[G2D_R_MASTER_BRIGHT]);
    const unsigned hofs = j->regs[G2D_R_BGHOFS] & 511;
    const g2d_v8 ones = g2d_v8_splat(0xffff), zero = g2d_v8_splat(0);
    const uint32_t tag[2] = { G2D_TAG_OUT + p, G2D_TAG_3D + p };
    for (unsigned v = 0; v < 32; v++) {
        const unsigned half = (v >> 2) & 1;
        g2d_v8 col[2], id[2], meta, op[5], f[16];
        const g2d_v8 fx = win ? v8_nz(c->allow[5][v]) : ones;
        if ((v & 3) == 0) g2d_wait(1u << tag[half]);
        hd_pair_vec(c, j, p, ent, nent, v, 0, win, col, id, &meta);
        hd_operation_vec(j, col, id, meta, fx, op);
        {
            /* color sin BG0: brillo del efecto solo donde el carril lo pide, luego el maestro */
            const unsigned mode = (j->regs[G2D_R_BLDCNT] >> 6) & 3;
            g2d_v8 b = op[1];
            if (mode >= 2)
                b = g2d_v8_sel(b, hd_bright_vec(b, hd_effect(mode - 1, j->regs[G2D_R_BLDY])), v8_nz(op[4]));
            b = hd_bright_vec(b, master);
            f[0] = CH5(b, 0); f[1] = CH5(b, 1); f[2] = CH5(b, 2);
        }
        hd_pair_vec(c, j, p, ent, nent, v, 1, win, col, id, &meta);
        hd_operation_vec(j, col, id, meta, fx, op);
        f[3] = op[0]; f[4] = CH5(op[1], 0); f[5] = CH5(op[1], 1); f[6] = CH5(op[1], 2);
        f[7] = op[2]; f[8] = op[3]; f[9] = op[4]; f[10] = g2d_v8_splat((uint16_t)master);
        f[11] = g2d_v8_splat((uint16_t)(hofs & 255)); f[12] = g2d_v8_splat((uint16_t)(hofs >> 8));
        f[13] = f[14] = f[15] = zero;
        hd_store_vec(&buf[(v & 7) * 8], f);
        if ((v & 3) == 3)
            g2d_put(&buf[half * 32], ea + (v - 3) * 8 * sizeof *buf, 32 * sizeof *buf, tag[half]);
    }
    g2d_wait((1u << tag[0]) | (1u << tag[1]));
}
static void hd_export(gpu2d_ctx *c, const g2d_job *j, unsigned p,
                       const int *ent, int nent, uint64_t ea, int scalar)
{
#ifdef __SPU__
    /* El SPE solo lleva la ruta vectorial (código y pila); la escalar queda en el PPE y el host como referencia. */
    (void)scalar;
    hd_export_vec(c, j, p, ent, nent, ea);
#else
    /* draw_3d ya consumió la línea desplazada: reutilizarla sin crecer LS.
     * Esperar cada trozo antes de volver a escribirlo. */
    g2d_hd_pixel *buf = (g2d_hd_pixel *)(void *)c->l3d_sh;
    int win = (j->dc & (G2D_DC_WIN0 | G2D_DC_WIN1 | G2D_DC_OBJWIN)) != 0;
    if (!scalar) { hd_export_vec(c, j, p, ent, nent, ea); return; }
    for (unsigned x0 = 0; x0 < 256; x0 += 64) {
        for (unsigned i = 0; i < 64;) {
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
            i++;
        }
        g2d_put(buf, ea + x0 * sizeof *buf, 64 * sizeof *buf, G2D_TAG_OUT + p);
        g2d_wait(1u << (G2D_TAG_OUT + p));
    }
#endif
}
