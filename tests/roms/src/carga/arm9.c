/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM carga, lado ARM9: trabajo continuo (kernel.h) para medir cuántas instrucciones por segundo corre
 * un intérprete con las dos CPUs ocupadas todo el tiempo. Repite iteraciones sin parar, compara cada resultado con
 * el esperado (gold.h, calculado en el host) y deja los contadores en la RAM principal (shared.h). En pantalla:
 * el número de iteraciones de cada CPU, los errores y una barra que avanza.
 *
 * Nunca espera una interrupción: el ARM9 no entra en HALT. Código propio de SalsaDS.
 */
#define K_THUMB __attribute__((target("thumb")))
#include <nds.h>

#include "../common/hw.h"
#include "gold.h"
#include "iter.h"
#include "shared.h"

#define FB ((volatile uint16_t *)0x06800000)

static struct k_buf buf9;

static void hex8(char *o, uint32_t v)
{
	for (int i = 0; i < 8; i++)
		o[i] = "0123456789ABCDEF"[(v >> (28 - 4 * i)) & 15];
}

int main(void)
{
	struct carga_shared *sh = CARGA_SHARED;
	char line[33];
	uint32_t it = 0;

	sds_nocache();
	MEM32(R_IME) = 0;
	sds_init("SALSADS CARGA");
	sh->a9_iter = sh->a9_bad = 0;
	sh->magic = CARGA_MAGIC;
	sds_print_row(2, "ARM9");
	sds_print_row(4, "ARM7");
	sds_print_row(6, "ERRORES");
	for (;;) {
		const uint32_t k = it % CARGA_SEEDS;
		const uint32_t r = carga_iter(&buf9, carga_seed[k]);

		it++;
		sh->a9_last = r;
		if (r != carga_gold[k])
			sh->a9_bad++;
		sh->a9_iter = it;
		hex8(line, it);
		line[8] = 0;
		sds_print_row(3, line);
		hex8(line, sh->a7_iter);
		line[8] = 0;
		sds_print_row(5, line);
		hex8(line, sh->a9_bad + sh->a7_bad);
		line[8] = 0;
		sds_print_row(7, line);
		/* barra: una columna por iteración, en la parte baja de la pantalla */
		for (int y = 176; y < 190; y++)
			FB[y * 256 + (it & 255u)] = (r != carga_gold[k]) ? (uint16_t)0x801Fu : (uint16_t)(0x8000u | ((it >> 3) & 31u) << 5);
	}
}
