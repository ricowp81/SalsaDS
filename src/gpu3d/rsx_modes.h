/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_MODES_H
#define SALSADS_RSX_MODES_H
#include "gpu3d/r3d_px.h"

enum { RSX_CLEAR, RSX_FOG, RSX_EDGE, RSX_TOON, RSX_HIGHLIGHT,
       RSX_SHADOW_MASK, RSX_SHADOW, RSX_EQUAL, RSX_WIRE, RSX_MODE_COUNT };
static const char *const rsx_mode_names[RSX_MODE_COUNT] = {
    "imagen-borrado", "niebla", "bordes", "toon", "resaltado",
    "mascara-sombra", "sombra", "igualdad", "alambre"
};
/* Presencia, no exclusión mutua: un cuadro puede necesitar varios modos.
 * No infiere visibilidad ni costo a partir de los atributos enviados. */
static inline uint32_t rsx_modes(const gx_frame *f, uint32_t flags)
{
    if (!f || (flags & R3D_F_BLANK)) return 0;
    uint32_t modes = 0, d = f->regs.disp3dcnt;
    if (d & D3_REARBMP) modes |= 1u << RSX_CLEAR;
    if ((d & D3_FOG) && !(flags & R3D_F_NOFOG)) modes |= 1u << RSX_FOG;
    if ((d & D3_EDGE) && !(flags & R3D_F_NOEDGE)) modes |= 1u << RSX_EDGE;
    for (uint32_t i = 0; i < f->npolys && i < GX_MAX_POLYS; i++) {
        uint32_t a = f->poly[i].attr;
        if (PA_MODE(a) == 2) modes |= 1u << ((d & D3_HIGHLIGHT) ? RSX_HIGHLIGHT : RSX_TOON);
        if (PA_MODE(a) == 3) modes |= 1u << (PA_ID(a) ? RSX_SHADOW : RSX_SHADOW_MASK);
        if (a & PA_DEQUAL) modes |= 1u << RSX_EQUAL;
        if (!PA_ALPHA(a)) modes |= 1u << RSX_WIRE;
    }
    return modes;
}
/* Tabla de 32 entradas que ocupa TEXUNIT1 en el dibujo de color de un
 * polígono. La de toon la lee toon.fcg (modo 2); la de niebla solo la ruta
 * por fragmento anterior (fog.fcg), que no convive con toon. La pasada final
 * de niebla la lee en TEXUNIT2 y la de atributos no la lee. Un polígono toon
 * con niebla final conserva la de toon: con la de niebla en su lugar, el
 * color toon sale de las densidades (R) con G = B = 0. */
enum { RSX_TABLE_NONE, RSX_TABLE_TOON, RSX_TABLE_FOG };
static inline unsigned rsx_color_table(unsigned mode, unsigned fog, unsigned fog_previous)
{
    if (mode == 2) return RSX_TABLE_TOON;
    return fog && fog_previous ? RSX_TABLE_FOG : RSX_TABLE_NONE;
}
#endif
