/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — ROM carga: una iteración completa y las semillas. */
#ifndef CARGA_ITER_H
#define CARGA_ITER_H

#include "kernel.h"

#define CARGA_N_ALU 3000u
#define CARGA_N_MUL 3000u
#define CARGA_SEEDS 4u

/* semillas de cada iteración (la iteración i usa seeds[i % CARGA_SEEDS] + base de la CPU) */
static const uint32_t carga_seed[CARGA_SEEDS] = { 0x1234ABCDu, 0x0BADF00Du, 0xC0FFEE11u, 0x5EED5EEDu };

static uint32_t carga_iter(struct k_buf *b, uint32_t seed)
{
	uint32_t r = k_alu(CARGA_N_ALU, seed);

	r = r * 31u + k_mem(b, seed + 1u);
	r = r * 31u + k_bytes(b, seed + 2u);
	r = r * 31u + k_mul(CARGA_N_MUL, seed + 3u);
	r = r * 31u + k_sort(b, seed + 4u);
	return r;
}

#endif
