/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.7 (c): el ARM7 en el hilo 1 del PPE.
 *
 * Dos hilos a la vez: el principal corre el ARM9 (un bucle traducido, en
 * trozos) y otro simula el ARM7, que está casi siempre en HALT: despierta,
 * hace un trabajito y vuelve a esperar. Se mide cuánto le quita al ARM9 cada
 * forma de esperar del hilo del ARM7 (girar, girar con prioridad baja de
 * SMT, dormir en pasos con sysUsleep, semáforo de lv2, cola de eventos de
 * lv2) y la latencia de despertarlo.
 */
#ifndef SALSADS_JIT_ARM_THREADS_H
#define SALSADS_JIT_ARM_THREADS_H

#include <stdint.h>

/* Corre un trozo del ARM9 y devuelve las instrucciones guest ejecutadas. */
typedef uint64_t (*arm9_chunk_fn)(void *ctx);

/* `quick`: ventanas cortas (RPCS3). Devuelve 0, o -1 si algo no se pudo crear. */
int threads_bench(arm9_chunk_fn chunk, void *ctx, double tb_hz, int quick);

#endif
