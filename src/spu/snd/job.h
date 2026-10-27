/* SPDX-License-Identifier: GPL-2.0-or-later */
/* SalsaDS — Fase 2.2: formato del trabajo entre el PPE y el SPE de audio (src/audio, src/spu/snd). */
#ifndef SALSADS_SPE_SND_JOB_H
#define SALSADS_SPE_SND_JOB_H

#include <stdint.h>

#define JOB_SND 1

/* Parámetros del trabajo (≤ 112 bytes): un cuadro de audio. Las direcciones de la RAM del ARM7 (regiones de 8 MB
 * 4..7: RAM principal y WRAM) van con su máscara; la captura escribe por la misma dirección. */
typedef struct job_snd {
	uint64_t state_ea;   /* snd_state, alineado a 128 */
	uint64_t log_ea;     /* snd_ent[nlog] */
	uint64_t out_ea;     /* float[2 * out_max] (salida a 48 kHz) */
	uint64_t nat_ea;     /* int16_t[2 * nticks] (mezclador a 32,7 kHz) */
	int64_t live_delta;  /* diagnóstico: dirección viva = rgn_ea + live_delta; 0 fuera de audio-verificar */
	uint32_t nlog, nticks, out_max, mode;
	float ratio_adj;
	uint32_t verify;
	uint64_t rgn_ea[4];
	uint32_t rgn_mask[4];
} job_snd;

_Static_assert(sizeof(job_snd) == 112, "job_snd debe medir lo mismo en PPE y SPE y caber en spukit_job.data");

/* Resultado (≤ 112 bytes) */
typedef struct job_snd_result {
	uint32_t ticks;      /* duración total del trabajo en el SPE (decrementador) */
	uint32_t mix_ticks;  /* ... de lo que es snd_run */
	uint32_t out_n;      /* muestras estéreo de salida */
	uint32_t applied;    /* escrituras de registros aplicadas */
	uint32_t busy;
	uint32_t peak;
	int32_t rc;
	uint32_t state_bytes;
	uint32_t ram_reads;
	uint32_t ram_changed;
	uint32_t write_overflow;
} job_snd_result;

#endif
