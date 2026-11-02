/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_EDGE_H
#define SALSADS_RSX_EDGE_H
#include <stdint.h>
#include <string.h>
#include "gpu3d/r3d_px.h"

/* Atributos de la pasada auxiliar: RGB contiene toda la profundidad de 24
 * bits; A conserva OID y arista geométrica del último opaco (0..127).
 * Un translúcido escribe A=255 con MIN_ALPHA y mantiene ambos; solo
 * PA_TDEPTH reemplaza RGB. Las sombras son translúcidas también con alfa
 * 31. La niebla conserva su propia pasada auxiliar: MIN con 127 no borra
 * el bit 7 de un byte sin modificar los otros siete bits. */
enum {
    RSX_EDGE_ID_MASK = 63,
    RSX_EDGE_CANDIDATE = 64,
    RSX_EDGE_GROUPS = 8,
    RSX_EDGE_ROW_TEXELS = 256,
    RSX_EDGE_ROW_BYTES = RSX_EDGE_ROW_TEXELS * 4,
    RSX_EDGE_FLAT_TEXEL = R3D_H
};

static inline uint32_t rsx_edge_encode(uint32_t depth, uint32_t att)
{
    uint32_t a = AT_OID(att) | ((att & AT_EDGE) ? RSX_EDGE_CANDIDATE : 0u);
    return (a << 24) | (depth & 0xffffffu);
}

static inline uint32_t rsx_edge_depth(uint32_t value) { return value & 0xffffffu; }
static inline uint32_t rsx_edge_meta_id(uint32_t value) { return (value >> 24) & RSX_EDGE_ID_MASK; }

/* Fuente: GBATEK, "DS 3D Toon, Edge, Fog, Alpha-Blending, Anti-Aliasing".
 * El candidato debe proceder de una arista geométrica: un hueco interior
 * de textura o un cruce de superficies no crea una arista nueva. El fondo
 * y los vecinos exteriores se codifican con clear_attr/clear_depth. */
static inline int rsx_edge_mark(uint32_t center, const uint32_t neighbors[4])
{
    if (!((center >> 24) & RSX_EDGE_CANDIDATE)) return 0;
    const uint32_t id = rsx_edge_meta_id(center), depth = rsx_edge_depth(center);
    for (unsigned i = 0; i < 4; i++)
        if (rsx_edge_meta_id(neighbors[i]) != id && depth < rsx_edge_depth(neighbors[i])) return 1;
    return 0;
}

/* La pasada sustituye RGB y conserva el alfa ya mezclado. Se ejecuta
 * después de todos los polígonos, antes de la niebla, como r3d_edge_row.
 * El AA pendiente del raster de referencia no modifica este resultado. */
static inline uint32_t rsx_edge_apply(const r3d_rstate *rs, uint32_t color, uint32_t center,
                                    const uint32_t neighbors[4])
{
    if (!rsx_edge_mark(center, neighbors)) return color;
    return (rs->edge[rsx_edge_meta_id(center) >> 3] & 0xffffffu) | (color & 0xff000000u);
}

/* Grupos independientes de respaldo: W, translúcidos/sombras, niebla.
 * La textura decal conserva el alfa del polígono; una textura de alfa
 * variable solo puede introducir translúcidos con texturas habilitadas. */
static inline unsigned rsx_edge_group(const gx_frame *f, uint32_t flags)
{
    unsigned group = (f->swap_params >> 1) & 1u;
    if ((f->regs.disp3dcnt & D3_FOG) && !(flags & R3D_F_NOFOG)) group |= 4u;
    for (unsigned i = 0; i < f->npolys && i < GX_MAX_POLYS; i++) {
        const gx_poly *p = &f->poly[i];
        const unsigned alpha = PA_ALPHA(p->attr), mode = PA_MODE(p->attr), fmt = (p->texparam >> 26) & 7u;
        if (mode == 3 || (alpha > 0 && alpha < 31) ||
            ((f->regs.disp3dcnt & D3_TEXTURE) && mode != 1 && (fmt == 1 || fmt == 6))) group |= 2u;
    }
    return group;
}

/* Una fila de textura por polígono, 256 texeles RGBA8 sin normalizar:
 * 0..191 guardan xle/xrs de 16 bits; 192 guarda Z plana y su indicador.
 * Las coordenadas se limitan a 0..256: el fragmento consulta un x nativo
 * de 0..255. alledge usa xle=256, xrs=0 y una fila vacía el inverso.
 * El llamador debe preparar p con r3d_poly_setup, no la preparación breve
 * del RSX: aquí se necesita la forma exacta de las cadenas de aristas. */
static inline void rsx_edge_span_encode(uint8_t rgba[4], const r3d_row *row)
{
    int32_t left = row ? row->xle : 0, right = row ? row->xrs : R3D_W;
    if (row && row->alledge) { left = R3D_W; right = 0; }
    left = left < 0 ? 0 : left > R3D_W ? R3D_W : left;
    right = right < 0 ? 0 : right > R3D_W ? R3D_W : right;
    rgba[0] = (uint8_t)((uint32_t)left >> 8); rgba[1] = (uint8_t)left;
    rgba[2] = (uint8_t)((uint32_t)right >> 8); rgba[3] = (uint8_t)right;
}

static inline int rsx_edge_span_test(const uint8_t rgba[4], unsigned x)
{
    unsigned left = (unsigned)rgba[0] * 256u + rgba[1], right = (unsigned)rgba[2] * 256u + rgba[3];
    return x < left || x >= right;
}

static inline void rsx_edge_spans(uint8_t out[RSX_EDGE_ROW_BYTES], const r3d_poly *p)
{
    memset(out, 0, RSX_EDGE_ROW_BYTES);
    for (int y = 0; y < R3D_H; y++) {
        r3d_row row;
        rsx_edge_span_encode(out + y * 4, r3d_row_at(p, y, &row) ? &row : NULL);
    }
    const uint32_t z = p->v[0].z;
    unsigned flat = 1;
    for (int i = 1; i < p->n; i++) if (p->v[i].z != z) flat = 0;
    out[RSX_EDGE_FLAT_TEXEL * 4] = (uint8_t)(z >> 16);
    out[RSX_EDGE_FLAT_TEXEL * 4 + 1] = (uint8_t)(z >> 8);
    out[RSX_EDGE_FLAT_TEXEL * 4 + 2] = (uint8_t)z;
    out[RSX_EDGE_FLAT_TEXEL * 4 + 3] = (uint8_t)flat;
}
#endif
