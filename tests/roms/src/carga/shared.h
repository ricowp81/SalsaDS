/* SalsaDS — ROM carga: el bloque de la RAM principal donde las dos CPUs dejan sus contadores. */
#ifndef CARGA_SHARED_H
#define CARGA_SHARED_H

#include <stdint.h>

struct carga_shared {
	volatile uint32_t magic;   /* 0x00 CARGA_MAGIC cuando el ARM9 arrancó */
	volatile uint32_t a9_iter; /* 0x04 iteraciones del ARM9 */
	volatile uint32_t a9_bad;  /* 0x08 iteraciones del ARM9 con resultado distinto del esperado */
	volatile uint32_t a7_iter; /* 0x0C */
	volatile uint32_t a7_bad;  /* 0x10 */
	volatile uint32_t a9_last; /* 0x14 resultado de la última iteración del ARM9 */
	volatile uint32_t a7_last; /* 0x18 */
};

#define CARGA_SHARED ((struct carga_shared *)0x023A0000u)
#define CARGA_MAGIC 0x47524143u /* "CARG" */

#endif
