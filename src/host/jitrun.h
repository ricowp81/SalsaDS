/* SalsaDS — el recompilador del ARM9 en salsads-cpurun (jitrun.c). */
#ifndef SALSADS_HOST_JITRUN_H
#define SALSADS_HOST_JITRUN_H

#include <stdint.h>

struct nds;

/* Conecta el recompilador al ARM9 de `n` (0 = bien; -1 = este host no ejecuta PowerPC64). */
int hj_attach(struct nds *n, int strict, int no_link, int far, int no_jl, void (*ev)(void *, int, uint64_t));
/* --lockstep: `m` con el recompilador (estricto y sin enlaces), `r` la referencia con el intérprete. */
int hj_lockstep_attach(struct nds *m, struct nds *r);
void hj_lockstep_frame(void);
void hj_lockstep_cycles(uint64_t k);
int hj_fail(void);
void hj_print_stats(struct nds *n);

#endif
