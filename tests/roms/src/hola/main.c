/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM «hola»: escribe en la VRAM en el modo de visualización
 * directa y reporta OK por el marco de pruebas (common/sds.h).
 *
 * Además de las líneas de texto, pinta un degradado de 256×192 con el
 * banco A y comprueba que la VRAM guarda lo escrito (lectura de vuelta de
 * tres tamaños de acceso, 8, 16 y 32 bits, en little-endian).
 */
#include <stdint.h>

#include "../common/sds.h"

#define FB ((volatile uint16_t *)0x06800000)

int main(void)
{
	sds_init("HOLA SALSADS");

	/* Franja de degradado en la parte de abajo de la pantalla. */
	for (int y = 100; y < 190; y++)
		for (int x = 0; x < 256; x++)
			FB[y * 256 + x] = (uint16_t)(0x8000 | ((x >> 3) << 10) | (((y - 100) >> 2) << 5) | (31 - (x >> 3)));

	sds_group("VRAM");
	FB[0] = 0xBEEF;
	FB[1] = 0x1234;
	SDS_CHECK(FB[0], 0xBEEF);
	SDS_CHECK(*(volatile uint32_t *)&FB[0], 0x1234BEEFu); /* little-endian */
	SDS_CHECK(*(volatile uint8_t *)&FB[0], 0xEF);
	SDS_CHECK(*(volatile uint8_t *)((uintptr_t)&FB[0] + 1), 0xBE);
	FB[0] = 0x8000 | (2 << 10) | (2 << 5) | 8; /* el color del fondo, que se ve arriba a la izquierda */
	FB[1] = FB[0];

	sds_group("SUMA");
	uint32_t acc = 0;
	for (uint32_t i = 1; i <= 100; i++)
		acc += i;
	SDS_CHECK(acc, 5050);

	sds_print_row(10, "HOLA MUNDO DS");
	sds_finish();
}
