/*
 * SalsaDS — Fase 0.4: rasterizador de referencia y comparación de imágenes.
 *
 * La referencia es la definición, escrita para leerse y no para correr:
 * recorre la caja de cada triángulo píxel a píxel, evalúa los bordes en
 * enteros de 64 bits y los atributos con coordenadas baricéntricas en doble
 * precisión, y lee cada texel directo de la VRAM (sin caché ni
 * decodificación previa). Misma semántica que el núcleo: regla de
 * arriba-izquierda, z-buffer de 24 bits con prueba «menor que», alfa 0 no se
 * dibuja, modulación y mezcla de la DS, translúcidos sin escribir
 * profundidad salvo R3D_ATTR_DEPTHWRITE.
 */
#ifndef SALSADS_R3D_REF_H
#define SALSADS_R3D_REF_H

#include <stdint.h>

#include "r3d.h"

struct r3d_ref_in {
	const struct r3d_tri *tris;
	uint32_t ntris;
	const uint8_t *vram_tex;
	uint32_t vram_tex_size;
	const uint8_t *vram_pal;
	uint32_t vram_pal_size;
	uint32_t scale;
	uint32_t clear_color, clear_depth;
	int idmode;
};

/* color y depth: W×H palabras cada uno. */
void r3d_ref_render(const struct r3d_ref_in *in, uint32_t *color, uint32_t *depth);

/* Resultado de comparar dos imágenes de r3d_rgba: iguales, cerca (ningún
 * canal difiere en más de R3D_CLOSE) y lejos. */
#define R3D_CLOSE 2

struct r3d_cmp {
	uint32_t exact, close, far;
	uint32_t first_far;   /* índice del primer píxel lejano (o n) */
	uint32_t maxdiff;
};

void r3d_compare(const uint32_t *got, const uint32_t *want, uint32_t n, struct r3d_cmp *c);

/* FNV-1a de 64 bits sobre los valores (no depende del orden de bytes). */
uint64_t r3d_hash(const uint32_t *p, uint32_t n);

#endif
