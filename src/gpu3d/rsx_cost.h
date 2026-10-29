/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SALSADS_RSX_COST_H
#define SALSADS_RSX_COST_H
#include <stdint.h>

/* Ventanas de costo 3D y trabajo del cuadro, sin la espera del flip.
 * GPU completa + preparación PPE frente al mayor tiempo de los dos SPE.
 * Las muestras tardías llevan generación y no contaminan la ruta siguiente. */
enum { RSX_COST_BASE, RSX_COST_PROBE, RSX_COST_SPE, RSX_COST_RSX };
enum { RSX_COST_WINDOW = 8, RSX_COST_HOLD = 120,
       RSX_COST_BUDGET = 16667, RSX_COST_RETURN = 16000 };
enum { RSX_COST_UNKNOWN, RSX_COST_FITS, RSX_COST_MARGIN,
       RSX_COST_OVER, RSX_COST_NO_GAIN, RSX_COST_STALE, RSX_COST_REASONS };
/* spe_age: cuadros (con o sin 3D) desde que se midió la referencia SPE; lo
 * sube el dueño en cada cuadro. Una referencia de otra escena (más vieja
 * que RSX_COST_HOLD) no sirve para bajar al SPE: primero se recalibra, salvo
 * allow_stale (A/B: comportamiento de la ronda 6). */
typedef struct rsx_cost {
    uint32_t epoch, phase, samples, frame_samples, age, streak, reason;
    uint32_t spe_age, allow_stale;
    uint64_t sum, frame_sum, rsx_us, spe_us, rsx_frame_us, spe_frame_us;
} rsx_cost;

static inline int rsx_cost_spe(const rsx_cost *c)
{
    return c->phase == RSX_COST_BASE || c->phase == RSX_COST_SPE;
}
static inline void rsx_cost_phase(rsx_cost *c, unsigned phase)
{
    c->phase=phase; c->epoch++; c->samples=c->frame_samples=c->age=c->streak=0;
    c->sum=c->frame_sum=0;
}
/* HD tiene prioridad si cabe. Para bajar, tanto el costo 3D como el trabajo
 * total deben favorecer al SPE: +10 %/0,5 ms y +5 %/0,5 ms respectivamente.
 * El máximo del trabajo PPE y del costo 3D impide declarar que cabe una GPU
 * lenta solo porque su espera quedó oculta en el flip. Es una cota de carga,
 * no una suma: el raster se solapa con la emulación. */
static inline int rsx_cost_update(rsx_cost *c)
{
    if (c->samples < RSX_COST_WINDOW || c->frame_samples < RSX_COST_WINDOW) return 0;
    uint64_t mean=c->sum/c->samples, frame=c->frame_sum/c->frame_samples;
    if (frame < mean) frame=mean;
    c->sum=c->frame_sum=0; c->samples=c->frame_samples=0;
    if (rsx_cost_spe(c)) { c->spe_us=mean; c->spe_frame_us=frame; c->spe_age=0; }
    else { c->rsx_us=mean; c->rsx_frame_us=frame; }
    unsigned next=c->phase;
    if (c->phase == RSX_COST_BASE) next=RSX_COST_PROBE;
    else if (c->phase == RSX_COST_SPE) {
        if (c->age >= RSX_COST_HOLD) next=RSX_COST_PROBE;
    } else {
        int over=frame > RSX_COST_BUDGET;
        int gain=mean*100 > c->spe_us*110+50000 &&
                 frame*100 > c->spe_frame_us*105+50000;
        /* La referencia SPE puede ser de otra escena (cuadros sin 3D no la
         * renuevan): si decidiría bajar, medir primero el SPE en esta. */
        int stale=over && gain && !c->allow_stale && c->spe_age > RSX_COST_HOLD;
        c->reason=frame <= RSX_COST_RETURN ? RSX_COST_FITS :
                  !over ? RSX_COST_MARGIN : stale ? RSX_COST_STALE :
                  gain ? RSX_COST_OVER : RSX_COST_NO_GAIN;
        if (stale) next=RSX_COST_BASE;
        else if (c->phase == RSX_COST_PROBE) {
            if (over && gain) next=RSX_COST_SPE;
            else if (c->reason == RSX_COST_MARGIN) c->streak=0;
            else if (++c->streak == 2) next=RSX_COST_RSX;
        } else {
            c->streak=over && gain ? c->streak+1 : 0;
            if (c->streak == 2) next=RSX_COST_SPE;
            else if (c->age >= RSX_COST_HOLD) next=RSX_COST_BASE;
        }
    }
    if (next == c->phase) return 0;
    rsx_cost_phase(c,next);
    return 1;
}
/* 1 solo cuando cambia la fase; cero us o >1 s son marcas inválidas/pausa.
 * Ambas ventanas deben pertenecer a la ruta y generación actuales. */
static inline int rsx_cost_sample(rsx_cost *c, uint32_t epoch, int spe, uint64_t us)
{
    if (epoch != c->epoch || spe != rsx_cost_spe(c) || !us || us > 1000000) return 0;
    c->sum+=us; c->samples++;
    return rsx_cost_update(c);
}
static inline int rsx_cost_frame(rsx_cost *c, uint32_t epoch, int spe, uint64_t us)
{
    if (epoch != c->epoch || spe != rsx_cost_spe(c) || !us || us > 1000000) return 0;
    c->frame_sum+=us; c->frame_samples++; c->age++;
    return rsx_cost_update(c);
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
