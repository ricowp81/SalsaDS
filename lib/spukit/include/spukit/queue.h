/* SPDX-License-Identifier: MIT */
/*
 * spukit — formato de la cola de trabajos, compartido por el PPE y el SPU.
 *
 * Cada trabajador (un hilo SPU) tiene un bloque de control en memoria
 * principal, alineado a 128 bytes (línea de caché del Cell y unidad de DMA
 * cómoda):
 *
 *   jobs[SPUKIT_QUEUE_LEN]     anillo de descriptores; los escribe el PPE
 *   results[SPUKIT_QUEUE_LEN]  resultado de cada trabajo; los escribe el SPU
 *   head                       último número de secuencia publicado y la
 *                              orden de salir; lo escribe el PPE
 *   status                     último trabajo terminado; lo escribe el SPU
 *
 * Los trabajos se numeran desde 1; el descriptor y el resultado del trabajo
 * `seq` están en la posición seq % SPUKIT_QUEUE_LEN. Un trabajo es grande a
 * propósito (una lista de comandos completa, un cuadro de audio): la cola es
 * corta y el timbre (mailbox) se toca una vez por trabajo, nunca por
 * elemento.
 */
#ifndef SPUKIT_QUEUE_H
#define SPUKIT_QUEUE_H

#include <stdint.h>

#define SPUKIT_QUEUE_LEN  8u    /* potencia de 2 */
#define SPUKIT_DATA_SIZE  112u  /* parámetros de un trabajo */
#define SPUKIT_RESULT_SIZE 112u /* resultado de un trabajo */

/* Descriptor de trabajo: 128 bytes. `kind` y `data` los define cada
 * programa SPU. */
typedef struct spukit_job {
	uint32_t seq;
	uint32_t kind;
	uint32_t size;  /* bytes válidos de data */
	uint32_t reserved;
	uint8_t data[SPUKIT_DATA_SIZE];
} __attribute__((aligned(128))) spukit_job;

/* Resultado: 128 bytes. `ticks` son del decrementador del SPU (frecuencia
 * del timebase: 79,8 MHz en la PS3). */
typedef struct spukit_result {
	uint32_t seq;
	int32_t error;  /* 0 = bien; el programa define el resto */
	uint32_t ticks; /* duración del trabajo en el SPU */
	uint32_t reserved;
	uint8_t data[SPUKIT_RESULT_SIZE];
} __attribute__((aligned(128))) spukit_result;

typedef struct spukit_head {
	volatile uint32_t posted;  /* último seq publicado */
	volatile uint32_t quit;    /* != 0: salir al vaciar la cola */
	uint32_t pad[30];
} __attribute__((aligned(128))) spukit_head;

typedef struct spukit_status {
	volatile uint32_t done;    /* último seq terminado (resultado escrito) */
	volatile uint32_t wakeups; /* veces que el SPU despertó por el timbre */
	uint32_t pad[30];
} __attribute__((aligned(128))) spukit_status;

typedef struct spukit_ctrl {
	spukit_job jobs[SPUKIT_QUEUE_LEN];
	spukit_result results[SPUKIT_QUEUE_LEN];
	spukit_head head;
	spukit_status status;
} __attribute__((aligned(128))) spukit_ctrl;

#define SPUKIT_SLOT(seq) ((seq) & (SPUKIT_QUEUE_LEN - 1))

#endif
