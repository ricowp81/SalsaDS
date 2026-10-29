/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_EXPORT_H
#define SALSADS_RSX_EXPORT_H
#include "gpu3d/r3d.h"
/* Contrato explícito de la superficie XDR; independiente del host. */
static inline uint32_t r3d_rsx_unpack(const uint8_t b[4])
{
    return R3D_PX(b[3], b[2], b[1], b[0]);
}
static inline void r3d_rsx_pack(uint8_t b[4], uint32_t p)
{
    b[0] = (uint8_t)R3D_PX_A(p); b[1] = (uint8_t)R3D_PX_B(p);
    b[2] = (uint8_t)R3D_PX_G(p); b[3] = (uint8_t)R3D_PX_R(p);
}
#endif
