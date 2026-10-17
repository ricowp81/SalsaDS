/*
 * SalsaDS — Fase 0.4: escena sintética (PPE y host).
 *
 * Todo en enteros con un generador propio: la misma escena en la Mac, en
 * qemu-ppc64 y en la PS3.
 *
 * Texturas: `ntex` texturas de la DS en la VRAM (little-endian), en tres
 * formatos (directo A1BGR5, paleta de 256 y de 16), de 32×32, 64×64 y 64×32,
 * con texeles transparentes, repetidas o recortadas («clamp») por eje, y
 * algunas con offsets alineados a 8 y no a 16. Con 12 texturas caben enteras
 * en la caché del SPE (88 KB decodificadas); con 24, no (176 KB).
 *
 * Triángulos: N con área media 3·256·192/N px² (cubren la pantalla unas 3
 * veces, sea cual sea N), centros al azar, vértices hasta 32 px fuera de la
 * pantalla, w entre 1 y 6 (perspectiva), color de vértice, una textura cada
 * uno y un 25 % translúcidos. Cada triángulo tiene su propia capa de z (una
 * permutación al azar) con poca pendiente: no hay dos que se crucen ni
 * empates de profundidad, así que qué triángulo se ve en cada píxel no
 * depende del redondeo y se puede comparar exacto. Orden de dibujo: los
 * opacos y después los translúcidos, como la DS.
 */
#ifndef SALSADS_R3D_SCENE_H
#define SALSADS_R3D_SCENE_H

#include <stdint.h>

#include "r3d.h"

#define R3D_VRAM_TEX_SIZE (512u * 1024u)
#define R3D_VRAM_PAL_SIZE (16u * 1024u)
#define R3D_SCENE_MAX_TEX 24u

struct r3d_texdef {
	uint32_t param;   /* TEXIMAGE_PARAM completo (con repetir y color 0) */
	uint16_t pltt;
};

/* Arma `ntex` (≤ R3D_SCENE_MAX_TEX) texturas; la VRAM se pone en cero antes.
 * Devuelve los bytes de VRAM de texturas usados. */
uint32_t r3d_scene_textures(uint8_t *vram_tex, uint8_t *vram_pal, unsigned ntex, struct r3d_texdef *defs);

/* N triángulos (≤ R3D_MAX_TRIS) en `out`. Devuelve el área total en px² a 1×. */
uint64_t r3d_scene_tris(struct r3d_tri *out, unsigned n, uint32_t seed, const struct r3d_texdef *defs,
			unsigned ntex);

#endif
