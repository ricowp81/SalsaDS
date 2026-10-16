/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — texto con una fuente de mapa de bits sobre una imagen en memoria
 * principal (un «lienzo»), para menús y mensajes en pantalla. La imagen se
 * presenta después como cualquier otra: rsxkit_fb_upload + rsxkit_fb_draw.
 *
 * Fuente: la «fixed» 8x13 de X11 (dominio público), celda de 8 × 13
 * píxeles, con ASCII, Latin-1 y unos símbolos para menús (flechas,
 * triángulos, puntos, rayas, «…»). El texto va en UTF-8; lo que la fuente no
 * tiene se dibuja como «?» y cada byte de una secuencia inválida, como «�».
 *
 * No usa el RSX ni PSL1GHT: compila igual en cualquier host (pruebas). No
 * asigna memoria. Los píxeles se escriben en el orden de bytes de la PS3
 * (big-endian) sea cual sea el host, como los lee rsxkit_fb_upload.
 */
#ifndef RSXKIT_TEXT_H
#define RSXKIT_TEXT_H

#include <stdint.h>

#include "rsxkit.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSXKIT_FONT_W 8
#define RSXKIT_FONT_H 13
#define RSXKIT_FONT_ASCENT 11  /* filas sobre la línea base */

typedef struct rsxkit_canvas {
	void *pixels;
	uint32_t width, height;
	uint32_t pitch;          /* bytes por línea */
	rsxkit_format format;    /* cualquiera de rsxkit_format */
} rsxkit_canvas;

/* Color 0xRRGGBB en el formato del lienzo (alfa opaco). */
uint32_t rsxkit_color(rsxkit_format format, uint32_t rgb);

/* Rellena el rectángulo (recortado al lienzo) con un color 0xRRGGBB. */
void rsxkit_fill(const rsxkit_canvas *c, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb);

/* Caracteres (celdas) de una cadena UTF-8. */
uint32_t rsxkit_text_cells(const char *utf8);

/* ¿La fuente tiene este punto de código? */
int rsxkit_font_has(uint32_t codepoint);

/* Dibuja `utf8` con la esquina superior izquierda de la primera celda en
 * (x, y), cada píxel de la fuente como un cuadro de scale × scale (scale 0
 * vale 1). Solo pinta los píxeles de los glifos (el fondo queda como
 * estaba) y recorta en los bordes del lienzo. Si la cadena tiene más de
 * `max_cells` celdas (0 = sin límite), dibuja max_cells − 1 y «…». Devuelve
 * las celdas dibujadas. */
uint32_t rsxkit_text_draw(const rsxkit_canvas *c, int32_t x, int32_t y, const char *utf8, uint32_t max_cells,
			  uint32_t rgb, uint32_t scale);

#ifdef __cplusplus
}
#endif

#endif
