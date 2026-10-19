/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — ROM carga, lado ARM7: el mismo trabajo que el ARM9 (kernel.h) sin parar, con sus resultados comparados
 * contra los mismos valores esperados. No usa interrupciones ni HALT.
 */
#define K_THUMB __attribute__((target("thumb")))
#include <stdint.h>

#include "gold.h"
#include "iter.h"
#include "shared.h"

static struct k_buf buf7;

int main(void)
{
	struct carga_shared *sh = CARGA_SHARED;
	uint32_t it = 0;

	sh->a7_iter = sh->a7_bad = 0;
	for (;;) {
		const uint32_t k = it % CARGA_SEEDS;
		const uint32_t r = carga_iter(&buf7, carga_seed[k]);

		it++;
		sh->a7_last = r;
		if (r != carga_gold[k])
			sh->a7_bad++;
		sh->a7_iter = it;
	}
}
