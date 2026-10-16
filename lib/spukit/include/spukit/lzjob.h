/* SPDX-License-Identifier: MIT */
/*
 * spukit — trabajo de compresión LZ en un SPE (spukit/lz.h): lo comparten
 * el programa SPU (spu/lz.c) y el lado PPE (spukit/lzstream.h).
 *
 * Un trabajo comprime UN bloque (≤ SPUKIT_LZ_BLOCK bytes) de memoria
 * principal a otra zona de memoria principal, por DMA.
 */
#ifndef SPUKIT_LZJOB_H
#define SPUKIT_LZJOB_H

#include <stdint.h>

#include "spukit/lz.h"

#define SPUKIT_LZ_JOB_COMPRESS 1

enum {
	SPUKIT_LZ_ERR_KIND = 1,   /* kind desconocido */
	SPUKIT_LZ_ERR_ARG = 2,    /* largo, alineación o capacidad inválidos */
};

/* Entrada: `src_ea` y `dst_ea` alineados a 16. Se leen len redondeado a
 * múltiplo de 16 bytes desde src_ea (el relleno se ignora, pero la memoria
 * tiene que existir) y se escriben a lo más clen redondeado a 16 bytes en
 * dst_ea; `cap` ≥ SPUKIT_LZ_BOUND(len) + 16. */
struct spukit_lz_job {
	uint64_t src_ea;
	uint64_t dst_ea;
	uint32_t len;
	uint32_t cap;
};

/* Salida: bytes comprimidos escritos en dst_ea, o 0 si comprimir no ganó
 * nada (el bloque va tal cual) y no se escribió nada. */
struct spukit_lz_result {
	uint32_t clen;
};

#endif
