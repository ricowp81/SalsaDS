/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — los recompiladores del ARM9 y del ARM7 en salsads-cpurun (jitrun.c). */
#ifndef SALSADS_HOST_JITRUN_H
#define SALSADS_HOST_JITRUN_H

#include <stdint.h>

struct nds;

/* Conecta los recompiladores de `n`: el del ARM9 si arm9 y el del ARM7 si arm7 (0 = bien; -1 = este host no ejecuta
 * PowerPC64). */
int hj_attach(struct nds *n, int strict, int no_link, int far, int no_jl, int arm7, int arm9, void (*ev9)(void *, int, uint64_t),
	      void (*ev7)(void *, int, uint64_t));
/* --lockstep: `m` con los recompiladores (estrictos y sin enlaces), `r` la referencia con el intérprete. */
int hj_lockstep_attach(struct nds *m, struct nds *r, int arm7, int arm9);
void hj_lockstep_frame(void);
void hj_lockstep_cycles(uint64_t k);
int hj_fail(void);
void hj_print_stats(struct nds *n);

#endif
