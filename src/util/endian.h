/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — acceso a memoria little-endian de la máquina emulada.
 *
 * La DS es little-endian y la PS3 big-endian: todo acceso a la memoria del
 * juego pasa por estas funciones (nunca castear punteros a la memoria
 * emulada). Esta versión es la portable de referencia, por bytes, y da lo
 * mismo en cualquier host; la capa que se optimiza en la Fase 0.3
 * (lwbrx/stwbrx o carga normal + rotaciones) tiene que coincidir con ella
 * bit a bit, y las pruebas corren en little-endian (Mac) y big-endian (qemu).
 */
#ifndef SALSADS_UTIL_ENDIAN_H
#define SALSADS_UTIL_ENDIAN_H

#include <stdint.h>

static inline uint16_t rd16le(const void *p)
{
	const uint8_t *b = p;
	return (uint16_t)(b[0] | b[1] << 8);
}

static inline uint32_t rd32le(const void *p)
{
	const uint8_t *b = p;
	return (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}

static inline void wr16le(void *p, uint16_t v)
{
	uint8_t *b = p;
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
}

static inline void wr32le(void *p, uint32_t v)
{
	uint8_t *b = p;
	b[0] = (uint8_t)v;
	b[1] = (uint8_t)(v >> 8);
	b[2] = (uint8_t)(v >> 16);
	b[3] = (uint8_t)(v >> 24);
}

#endif
