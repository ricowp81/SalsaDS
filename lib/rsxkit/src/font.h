/* SPDX-License-Identifier: MIT */
/*
 * rsxkit — la fuente y el UTF-8, compartidos por text.c (lienzo de la CPU) y
 * ui.c (texto en el RSX). Interno; sin PSL1GHT.
 */
#ifndef RSXKIT_SRC_FONT_H
#define RSXKIT_SRC_FONT_H

#include <stdint.h>

/* Siguiente punto de código de *s (avanza *s); U+FFFD si la secuencia no
 * es UTF-8 válido (consume un byte). */
uint32_t rsxkit_utf8_next(const char **s);
/* Glifos de la tabla, en orden de punto de código. */
int rsxkit_glyph_total(void);
/* Índice del glifo de `cp` en la tabla, o el de «?» si no está. */
int rsxkit_glyph_index(uint32_t cp);
/* RSXKIT_FONT_H filas del glifo `index`; el bit 7 es la columna izquierda. */
const uint8_t *rsxkit_glyph_rows(int index);

#endif
