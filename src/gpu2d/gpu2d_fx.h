/*
 * SalsaDS — efectos de color de los motores 2D (Fase 2.1): mezcla alfa, brillo y brillo maestro.
 *
 * Los colores llegan en BGR555. La aritmética se hace, como en el hardware, con 6 bits por canal (el color de 5 bits
 * se extiende con un 0 abajo) y el resultado se queda con los 5 de arriba; el redondeo sale de las tablas de melonDS
 * 1.1: el programa gfxfx de tests/roms compara cada factor con cada color (17 factores × 32 colores, subir y bajar, y
 * 24 parejas de EVA/EVB) y estas fórmulas dan los mismos 5 bits en todos los casos. Lo comparten la referencia y el
 * compositor.
 */
#ifndef SALSADS_GPU2D_FX_H
#define SALSADS_GPU2D_FX_H

#include <stdint.h>

static inline int g2d_ev(int v)
{
	return v > 16 ? 16 : v;
}

static inline uint16_t g2d_blend(uint16_t a, uint16_t b, int eva, int evb)
{
	uint16_t r = 0;

	for (int k = 0; k < 3; k++) {
		int v = (2 * ((a >> (5 * k)) & 31) * eva + 2 * ((b >> (5 * k)) & 31) * evb + 8) >> 4;

		if (v > 63)
			v = 63;
		r |= (uint16_t)((v >> 1) << (5 * k));
	}
	return r;
}

static inline uint16_t g2d_bright_up(uint16_t a, int evy)
{
	uint16_t r = 0;

	for (int k = 0; k < 3; k++) {
		const int c6 = 2 * ((a >> (5 * k)) & 31);

		r |= (uint16_t)(((c6 + (((63 - c6) * evy + 8) >> 4)) >> 1) << (5 * k));
	}
	return r;
}

static inline uint16_t g2d_bright_down(uint16_t a, int evy)
{
	uint16_t r = 0;

	for (int k = 0; k < 3; k++) {
		const int c6 = 2 * ((a >> (5 * k)) & 31);

		r |= (uint16_t)(((c6 - ((c6 * evy + 7) >> 4)) >> 1) << (5 * k));
	}
	return r;
}

/* Capa 3D (Fase 3.2): píxel del motor de render (R3D_PX: 6 bits por canal, alfa en 24-28) a BGR555. */
static inline uint16_t g2d_3d_555(uint32_t px)
{
	return (uint16_t)(((px >> 1) & 31u) | (((px >> 9) & 31u) << 5) | (((px >> 17) & 31u) << 10));
}

/* 3D translúcido sobre el objetivo 2: (3D·(a + 1) + 2D·(31 − a)) / 32 en 6 bits por canal [melonDS]. */
static inline uint16_t g2d_blend3d(uint16_t a, uint16_t b, int alpha)
{
	uint16_t r = 0;

	for (int k = 0; k < 3; k++) {
		const int v = (2 * ((a >> (5 * k)) & 31) * (alpha + 1) + 2 * ((b >> (5 * k)) & 31) * (31 - alpha)) >> 5;

		r |= (uint16_t)((v >> 1) << (5 * k));
	}
	return r;
}

/* MASTER_BRIGHT: bits 0-4 factor, 14-15 modo (1 sube, 2 baja) */
static inline uint16_t g2d_master_bright(uint16_t c, uint16_t reg)
{
	const int mode = (reg >> 14) & 3, f = g2d_ev(reg & 31);

	if (mode == 1)
		return g2d_bright_up(c, f);
	if (mode == 2)
		return g2d_bright_down(c, f);
	return c;
}

#endif
