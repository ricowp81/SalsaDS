/* SPDX-License-Identifier: MIT */
/*
 * spukit — trabajos por lotes en los SPEs de la PS3 (PSL1GHT), lado PPE.
 *
 * Un trabajador es un hilo SPU persistente (su propio grupo, un SPE) que
 * duerme en su mailbox de entrada y atiende una cola de trabajos en memoria
 * principal (spukit/queue.h). El PPE publica un trabajo (un descriptor de
 * 128 bytes que apunta a sus datos), toca el timbre y sigue con lo suyo;
 * más tarde consulta o espera el resultado. Pensado para trabajos grandes
 * (una lista de comandos completa por cuadro), no para diálogos finos.
 *
 *     spukit_init(2);
 *     spukit_worker *w = spukit_worker_start(spu_prog_bin, "gfx", arg);
 *     uint32_t seq = spukit_post(w, KIND, &params, sizeof params);
 *     ...                                   (el PPE trabaja en paralelo)
 *     const spukit_result *r = spukit_wait(w, seq);
 *     spukit_worker_stop(w);
 *
 * El programa SPU usa spukit/spu.h. Todas las llamadas de un trabajador
 * desde el mismo hilo del PPE. Sin asignaciones después de
 * spukit_worker_start. Documentación en lib/spukit/README.md.
 */
#ifndef SPUKIT_SPUKIT_H
#define SPUKIT_SPUKIT_H

#include <stdint.h>

#include "spukit/queue.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SPUKIT_OK = 0,
	SPUKIT_EINIT = -1,   /* sysSpuInitialize */
	SPUKIT_EIMAGE = -2,  /* imagen SPU inválida */
	SPUKIT_ETHREAD = -3, /* grupo o hilo SPU */
	SPUKIT_ENOMEM = -4,
	SPUKIT_EARG = -5,
};

#define SPUKIT_MAX_WORKERS 6

/* Reserva `spus` SPEs para hilos SPU (6 como máximo en un juego). Una vez
 * por proceso, antes de crear trabajadores. */
int spukit_init(uint32_t spus);

typedef struct spukit_worker spukit_worker;

/* Arranca un trabajador con la imagen ELF del SPU embebida en `elf`. El
 * programa recibe en main(ctrl_ea, arg, 0, 0) la dirección de su bloque de
 * control y `arg`. NULL si falla (spukit_last_error dice por qué). */
spukit_worker *spukit_worker_start(const void *elf, const char *name, uint64_t arg);
int spukit_last_error(void);

/* Publica un trabajo con `size` bytes de parámetros (≤ SPUKIT_DATA_SIZE)
 * y toca el timbre. Si la cola está llena, espera a que se libere un
 * lugar. Devuelve el número de secuencia (desde 1). */
uint32_t spukit_post(spukit_worker *w, uint32_t kind, const void *data, uint32_t size);

/* ¿Terminó el trabajo `seq`? No bloquea. */
int spukit_done(const spukit_worker *w, uint32_t seq);

/* Espera el trabajo `seq` y devuelve su resultado, válido hasta que se
 * publiquen SPUKIT_QUEUE_LEN trabajos más. */
const spukit_result *spukit_wait(spukit_worker *w, uint32_t seq);

/* Pide salir al vaciar la cola, espera al hilo y libera el grupo. Devuelve
 * el estado de salida del programa SPU (0 si terminó bien). */
int spukit_worker_stop(spukit_worker *w);

/* Estadísticas del trabajador desde el arranque. */
typedef struct spukit_stats {
	uint32_t posted;      /* trabajos publicados */
	uint32_t doorbells;   /* timbres (sysSpuThreadWriteMb) */
	uint32_t wakeups;     /* veces que despertó el SPU */
	uint32_t waits;       /* spukit_wait que tuvieron que esperar */
	uint64_t wait_us;     /* tiempo total esperando en spukit_wait */
	uint64_t full_us;     /* tiempo total esperando lugar en la cola */
} spukit_stats;

void spukit_get_stats(const spukit_worker *w, spukit_stats *out);

/* Frecuencia del decrementador del SPU (ticks de spukit_result) en Hz. */
uint64_t spukit_tick_hz(void);

#ifdef __cplusplus
}
#endif

#endif
