/* SPDX-License-Identifier: GPL-2.0-or-later */
/* El respaldo usa la reserva XDR de 448 celdas; permite alojar el código de
 * decodificación sin reducir la holgura de 8192 bytes. No cambia el raster normal.
 * Enlace: texto 66400 B, datos 800 B, BSS 172960 B; pila disponible 21840 B,
 * cadena máxima 8000 B, holgura 13840 B. La reserva de RGBA8 (<26 KiB) comparte
 * la del raster; sus trabajos son seriales y cada raster reinicia las cachés. */
#define R3D_XDR_ONLY 1
#define RSX_PREP 1
#include "../raster3d/raster3d.c"
