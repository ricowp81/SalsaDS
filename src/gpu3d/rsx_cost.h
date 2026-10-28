/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_COST_H
#define SALSADS_RSX_COST_H
#include <stdint.h>

/* Ventanas consecutivas, nunca alternancia por cuadro. Costos en us:
 * GPU completa + preparación PPE frente al mayor tiempo de los dos SPE.
 * Las muestras tardías llevan generación y no contaminan la ruta siguiente. */
enum { RSX_COST_BASE, RSX_COST_PROBE, RSX_COST_SPE, RSX_COST_RSX };
enum { RSX_COST_WINDOW = 8, RSX_COST_HOLD = 120 };
typedef struct rsx_cost {
    uint32_t epoch, phase, samples, age, streak;
    uint64_t sum, rsx_us, spe_us;
} rsx_cost;

static inline int rsx_cost_spe(const rsx_cost *c)
{
    return c->phase == RSX_COST_BASE || c->phase == RSX_COST_SPE;
}
static inline void rsx_cost_phase(rsx_cost *c, unsigned phase)
{
    c->phase=phase; c->epoch++; c->samples=c->age=c->streak=0; c->sum=0;
}
/* 1 solo cuando cambia la fase; cero us o >1 s son marcas inválidas/pausa.
 * La entrada al RSX exige dos ventanas con 15 % + 0,5 ms de margen;
 * la salida exige dos ventanas 10 % + 0,5 ms más caras. */
static inline int rsx_cost_sample(rsx_cost *c, uint32_t epoch, int spe, uint64_t us)
{
    if (epoch != c->epoch || spe != rsx_cost_spe(c) || !us || us > 1000000) return 0;
    c->sum+=us; c->age++;
    if (++c->samples != RSX_COST_WINDOW) return 0;
    uint64_t mean=c->sum/RSX_COST_WINDOW;
    c->sum=0; c->samples=0;
    if (spe) c->spe_us=mean; else c->rsx_us=mean;
    unsigned next=c->phase;
    if (c->phase == RSX_COST_BASE) next=RSX_COST_PROBE;
    else if (c->phase == RSX_COST_PROBE) {
        if ((mean+500)*100 > c->spe_us*85) next=RSX_COST_SPE;
        else if (++c->streak == 2) next=RSX_COST_RSX;
    } else if (c->phase == RSX_COST_SPE) {
        if (c->age >= RSX_COST_HOLD) next=RSX_COST_PROBE;
    } else {
        c->streak = mean*100 > c->spe_us*110+50000 ? c->streak+1 : 0;
        if (c->streak == 2) next=RSX_COST_SPE;
        else if (c->age >= RSX_COST_HOLD) next=RSX_COST_BASE;
    }
    if (next == c->phase) return 0;
    rsx_cost_phase(c,next);
    return 1;
}

/* La captura del núcleo consulta esta instantánea, no escrituras posteriores.
 * Ser conservador incluso si el banco no está en LCDC o solo se captura B. */
static inline int rsx_native_exact(uint32_t capture, int verify)
{
    return verify || (capture & 0x80000000u) != 0;
}
static inline int rsx_native_reduce(unsigned scale, int previous, int exact)
{
    return scale > 1 && !previous && !exact;
}
#endif
