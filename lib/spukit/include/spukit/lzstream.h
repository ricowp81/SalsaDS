/* SPDX-License-Identifier: MIT */
/*
 * spukit — compresión en flujo de un búfer grande (lado PPE): lo parte en
 * bloques de SPUKIT_LZ_BLOCK bytes, los comprime en un SPE (o, sin SPE, en
 * el PPE) y entrega cada bloque ya con su encabezado (spukit_lz_frame) a una
 * función de salida, en orden, mientras el SPE sigue con los siguientes.
 * Pensado para correr en un hilo de baja prioridad que escribe a disco: el
 * SPE comprime por delante de lo que el disco acepta, hasta
 * SPUKIT_LZS_SLOTS bloques.
 *
 *     spukit_lzs *z = spukit_lzs_open(spu_lz_bin);   // NULL: todo en el PPE
 *     ...
 *     spukit_lzs_run(z, buf, len, mi_salida, ctx, &stats);
 *     spukit_lzs_close(z);
 *
 * El flujo son los bloques uno detrás de otro: [encabezado de 4 bytes]
 * [contenido], con ceil(len / SPUKIT_LZ_BLOCK) bloques; el último puede ser
 * más corto. spukit_lz_decompress los lee uno por uno.
 *
 * Una sola llamada a la vez por spukit_lzs, desde un solo hilo (el mismo
 * que lo abrió o cualquiera, mientras no se mezclen). spukit_lzs_run no
 * asigna memoria: el anillo de salida se reserva en spukit_lzs_open.
 */
#ifndef SPUKIT_LZSTREAM_H
#define SPUKIT_LZSTREAM_H

#include <stdint.h>

#include "spukit/lz.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPUKIT_LZS_SLOTS 8

typedef struct spukit_lzs spukit_lzs;

/* Salida: recibe cada pedazo del flujo (encabezado de un bloque, y su
 * contenido). 0 si lo aceptó; otro valor aborta la compresión. */
typedef int (*spukit_lzs_sink)(void *ctx, const void *data, uint32_t len);

typedef struct spukit_lzs_stats {
	uint32_t blocks;      /* bloques entregados */
	uint32_t stored;      /* de ellos, sin comprimir (comprimir no ganó) */
	uint32_t spu_blocks;  /* comprimidos por el SPE */
	uint32_t ppe_blocks;  /* comprimidos por el PPE (sin SPE, o el SPE dio error) */
	uint32_t in_bytes;
	uint32_t out_bytes;   /* con los encabezados */
	uint64_t spu_ticks;   /* tiempo del SPE comprimiendo (spukit_tick_hz) */
	uint64_t wall_us;     /* de principio a fin de la llamada */
	uint64_t sink_us;     /* de eso, dentro de la función de salida */
} spukit_lzs_stats;

/* `spu_elf`: la imagen ELF del programa SPU de spukit/spu/lz.c embebida, o
 * NULL para comprimir solo en el PPE. Si el SPE no arranca, sigue en el PPE.
 * NULL solo si no hay memoria. */
spukit_lzs *spukit_lzs_open(const void *spu_elf);
void spukit_lzs_close(spukit_lzs *z);

/* ¿Hay un SPE trabajando? */
int spukit_lzs_has_spu(const spukit_lzs *z);

/* Comprime `len` bytes de `src` (alineado a 16, y legibles hasta el
 * siguiente múltiplo de 16) y los entrega a `sink`. Devuelve 0, o -1 si la
 * salida falló (el SPE termina lo que ya tenía publicado antes de volver).
 * `stats` puede ser NULL. */
int spukit_lzs_run(spukit_lzs *z, const void *src, uint32_t len, spukit_lzs_sink sink, void *ctx,
		   spukit_lzs_stats *stats);

/* Comprime un solo bloque en el PPE y lo deja en `out` (SPUKIT_LZ_BOUND(n) +
 * 16 bytes); devuelve los bytes comprimidos, o 0 si no ganó. Para pruebas
 * que comparan el PPE con el SPE. */
uint32_t spukit_lzs_ppe_block(spukit_lzs *z, const void *src, uint32_t n, void *out);

#ifdef __cplusplus
}
#endif

#endif
