/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_GPU2D_HD_H
#define SALSADS_GPU2D_HD_H
#include "gpu2d/gpu2d_fx.h"
/* Cuatro texeles RGBA8 por píxel nativo; bytes independientes del endian.
 * 0: salida sin BG0 (RGB5), operación con BG0 presente.
 * 1: operando RGB5, EVA. 2: EVB, brillo, maestro, HOFS bajo.
 * 3: HOFS alto; el resto reservado. Nunca contiene muestras HD. */
typedef struct g2d_hd_pixel { uint8_t b[16]; } g2d_hd_pixel;
enum { G2D_HD_CONSTANT = 1, G2D_HD_SAMPLE, G2D_HD_ALPHA, G2D_HD_BLEND };
#define G2D_HD_BYTES (256u * 192u * sizeof(g2d_hd_pixel))
static inline void g2d_hd_color(uint8_t *p, uint16_t c)
{
    p[0] = c & 31; p[1] = (c >> 5) & 31; p[2] = (c >> 10) & 31;
}
static inline uint16_t g2d_hd_rgb(const uint8_t *p)
{
    return p[0] | (p[1] << 5) | (p[2] << 10);
}
static inline uint16_t g2d_hd_bright(uint16_t c, unsigned mode)
{
    return mode >= 64 ? g2d_bright_down(c, mode & 31) :
           mode >= 32 ? g2d_bright_up(c, mode & 31) : c;
}
/* Oráculo portable para comparar el contrato de metadatos con el compositor. */
static inline uint16_t g2d_hd_eval(const g2d_hd_pixel *p, uint32_t sample)
{
    const uint8_t *b = p->b;
    unsigned a = (sample >> 24) & 31;
    uint16_t c = g2d_hd_rgb(b + 4), s = g2d_3d_555(sample);
    if (!a || !b[3]) return g2d_hd_rgb(b);
    if (b[3] == G2D_HD_SAMPLE) c = s;
    if (b[3] == G2D_HD_ALPHA) c = g2d_blend3d(s, c, a, g2d_3d_lsb(sample));
    if (b[3] == G2D_HD_BLEND) c = g2d_blend(c, s, b[7], b[8]);
    return g2d_hd_bright(g2d_hd_bright(c, b[9]), b[10]);
}
#endif
