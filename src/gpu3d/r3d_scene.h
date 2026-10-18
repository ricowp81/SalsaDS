/* SalsaDS — escenas sintéticas del render 3D (Fase 3.2): pruebas unitarias y autoprueba de la PS3. */
#ifndef SALSADS_GPU3D_R3D_SCENE_H
#define SALSADS_GPU3D_R3D_SCENE_H

#include "gpu3d/gx.h"
#include "gpu3d/r3d.h"

/* Arma con el motor de geometría `g` un cuadro de `npolys` polígonos al azar (formas, modos, alfas, ids, texturas de
 * los 7 formatos, perspectiva u ortográfica según la semilla) y registros del render al azar, y llena `vram`
 * (R3D_VRAM_ZERO_OFF + 128 bytes) y las ranuras. Devuelve el cuadro entregado (dentro de `g`). */
const gx_frame *r3d_scene_build(gx_t *g, uint32_t seed, uint32_t npolys, uint8_t *vram, uint32_t texslot[R3D_TEX_SLOTS],
				uint32_t palslot[R3D_PAL_SLOTS]);

#endif
