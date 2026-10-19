/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — distribución de las dos pantallas en la salida de video.
 *
 * Geometría pura (sin RSX), para probarla en el host: dado el tamaño de la
 * salida, dónde va cada pantalla de 256×192 y con qué escala. La escala
 * «entera» usa un múltiplo entero del tamaño de la DS (píxeles nítidos); la
 * «ajustada» usa la mayor escala, no entera, que cabe.
 */
#ifndef SALSADS_FRONTEND_LAYOUT_H
#define SALSADS_FRONTEND_LAYOUT_H

#define SDS_SCREEN_W 256
#define SDS_SCREEN_H 192

enum sds_layout {
	SDS_LAYOUT_SIDE,        /* lado a lado: arriba a la izquierda, abajo (táctil) a la derecha */
	SDS_LAYOUT_STACKED,     /* una sobre otra */
	SDS_LAYOUT_BIG_TOP,     /* grande la de arriba, chica la táctil a su derecha */
	SDS_LAYOUT_BIG_BOTTOM,  /* grande la táctil, chica la de arriba a su derecha */
	SDS_LAYOUT_COUNT,
};

enum sds_scale { SDS_SCALE_FIT, SDS_SCALE_INTEGER, SDS_SCALE_COUNT };

struct sds_rect {
	int x, y, w, h;   /* píxeles de la salida */
	float scale;      /* w / 256 */
};

/* out[0] = pantalla de arriba, out[1] = pantalla táctil (la de abajo). */
void sds_layout_compute(enum sds_layout layout, enum sds_scale scale, int out_w, int out_h, struct sds_rect out[2]);
const char *sds_layout_name(enum sds_layout layout);
const char *sds_scale_name(enum sds_scale scale);

#endif
