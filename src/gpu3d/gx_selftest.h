/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — secuencias de órdenes de prueba para el ejecutor del motor de geometría (Fase 3.5): un registro de órdenes
 * pseudoaleatorio pero determinista (matrices, pilas, luces, polígonos de los cuatro tipos, recortes, pruebas de caja). Lo usan
 * las pruebas unitarias y la autoprueba del SPE de geometría al arrancar (el SPE y el PPE ejecutan el mismo registro y los
 * resultados tienen que ser idénticos bit a bit).
 */
#ifndef SALSADS_GPU3D_GX_SELFTEST_H
#define SALSADS_GPU3D_GX_SELFTEST_H

#include <stdint.h>

/* Genera hasta `maxw` palabras de registro (órdenes completas) y devuelve cuántas. */
uint32_t gx_selftest_gen(uint32_t seed, uint32_t *log, uint32_t maxw, uint32_t steps);

#endif
