/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.4: E/S del núcleo del rasterizador.
 *
 * El núcleo (r3d_core.c) es el mismo en el SPE, el PPE y el host; lo que
 * cambia es cómo llegan los datos: por DMA del MFC en el SPE (spu_main.c) y
 * con memcpy en el PPE y el host (r3d_io_mem.c). Las direcciones de memoria
 * principal son `uint64_t`; las del local store, punteros. Tamaños múltiplos
 * de 16 y direcciones alineadas a 16. Las funciones no esperan, salvo
 * r3d_io_wait.
 */
#ifndef SALSADS_R3D_IO_H
#define SALSADS_R3D_IO_H

#include <stdint.h>

void r3d_io_get(void *ls, uint64_t ea, uint32_t size, unsigned tag);
void r3d_io_put(const void *ls, uint64_t ea, uint32_t size, unsigned tag);
/* Trae n elementos de `elem` bytes: el k-ésimo desde base + idx[k]·elem, uno
 * detrás de otro en `ls` (en el SPE, una lista de DMA; n ≤ R3D_CHUNK). */
void r3d_io_gather(void *ls, uint64_t base, const uint16_t *idx, unsigned n, uint32_t elem, unsigned tag);
/* Saca `rows` filas de `row_bytes` (contiguas en `ls`) a ea, ea + stride… */
void r3d_io_put_rows(const void *ls, uint64_t ea, unsigned rows, uint32_t row_bytes, uint32_t stride, unsigned tag);
void r3d_io_wait(uint32_t tag_mask);
/* Reloj que sube, en ticks del timebase (79,8 MHz en la PS3); 0 si no hay. */
uint32_t r3d_io_ticks(void);

#endif
