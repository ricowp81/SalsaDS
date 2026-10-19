/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — ROM demo: lo que el ARM7 deja al ARM9 en RAM principal cada cuadro. */
#ifndef DEMO_SHARED_H
#define DEMO_SHARED_H

#include <stdint.h>

struct demo_shared {
	volatile uint32_t magic;  /* 0x4F4D4544 */
	volatile uint32_t frame;  /* cuadros que vio el ARM7 */
	volatile uint32_t ext;    /* EXTKEYIN: X, Y, lápiz, tapa (0 = apretado salvo la tapa) */
	volatile uint32_t pen;    /* 1 si el lápiz toca */
	volatile int32_t px, py;  /* píxel de la pantalla táctil (de la calibración del firmware) */
	volatile uint32_t adc_x, adc_y;
};

#define DEMO_SHARED ((struct demo_shared *)0x023A0000u)
#define DEMO_MAGIC 0x4F4D4544u

#endif
