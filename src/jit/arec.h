/*
 * SalsaDS — recompilador dinámico ARMv5TE (ARM y Thumb) → PowerPC64 para el ARM9 (Fase 4).
 *
 * Código propio (GPL-2.0-or-later), escrito desde el manual de arquitectura de ARM, GBATEK, la Power ISA 2.02 y
 * lo medido en la PS3 (docs/fase0.md §0.3 y §0.7); el diseño sigue al recompilador MIPS → PPC64 de Salsa64
 * (bloques con enlace por parcheo, mapa de código para invalidar, caminos fríos aparte). Detalle en docs/fase4.md.
 *
 * Reemplaza a acore_run() para una CPU: mismo `struct acore`, mismas funciones de memoria para lo lento, mismas
 * rutinas de excepción. Lo que el traductor no sabe hacer lo ejecuta el intérprete desde el bloque (una
 * «llamada al intérprete» con los registros en memoria), así que cualquier programa corre.
 *
 * Dependencias: solo src/cpu (struct acore) y src/jit; lo propio de la DS (mapa de memoria, de dónde sale el
 * código) lo da el dueño por `struct arec_cfg` y arec_map_set (src/core/jit.c).
 *
 * El código generado es PowerPC64 big-endian (la PS3); en otro host el traductor compila y se prueba, pero
 * arec_create devuelve NULL (no hay dónde ejecutarlo) y el dueño sigue con el intérprete.
 */
#ifndef SALSADS_JIT_AREC_H
#define SALSADS_JIT_AREC_H

#include <stddef.h>
#include <stdint.h>

#include "cpu/acore.h"

#define AREC_PAGE_SHIFT 14u              /* páginas de 16 KB en la tabla de punteros */
#define AREC_PAGES (1u << (32 - AREC_PAGE_SHIFT))
#define AREC_PHYS_NONE 0xFFFFFFFFu       /* memoria de solo lectura o sin seguimiento */
#define AREC_LINE_SHIFT 6u               /* granularidad del mapa de código: 64 bytes */

struct arec;

/* Clases de instrucciones que ejecuta el intérprete desde un bloque (para las estadísticas). */
enum {
	AREC_CK_MRS, AREC_CK_MSR, AREC_CK_SWI, AREC_CK_CP15, AREC_CK_LDRD, AREC_CK_SWP, AREC_CK_QDSP, AREC_CK_BLOCK_S,
	AREC_CK_BLOCK_SLOW, AREC_CK_FLAGCOND, AREC_CK_UNDEF, AREC_CK_TH_SWI, AREC_CK_TH_SLOW, AREC_CK_OTHER,
	AREC_CALL_KINDS
};
extern const char *const arec_call_kind_name[AREC_CALL_KINDS];

struct arec_cfg {
	void *code;          /* memoria escribible y ejecutable para el código generado */
	size_t code_bytes;
	int strict;          /* 1: un bloque corre solo si cabe entero antes de `until` (mismos ciclos que el intérprete) */
	int no_link;         /* 1: sin enlaces entre bloques ni búsqueda de destinos en línea (cada bloque vuelve) */
	int no_inline_jl;    /* 1: sin búsqueda de destinos indirectos en línea (los enlaces directos siguen) */
	int far_calls;       /* 1: llamadas a C siempre por mtctr/bctrl (prueba el camino lejano) */
	/* Vacía la caché de instrucciones para [p, p + n) (dcbst/sync/icbi/isync en la PS3). */
	void (*icache_flush)(const void *p, size_t n);
	/* De dónde sale el código: puntero a los bytes de la instrucción en `a` (vista de captación, little-endian),
	 * su desplazamiento físico para invalidar (AREC_PHYS_NONE si no se escribe nunca) y cuántos bytes siguen en
	 * la misma memoria. NULL: no se traduce (el intérprete la ejecuta). */
	const uint8_t *(*code_ptr)(void *ctx, uint32_t a, uint32_t *phys, uint32_t *avail);
	void *ctx;
	uint32_t phys_size;  /* tamaño del espacio físico (múltiplo de 16 KB) */
	/* Para --lockstep: lo llama el despachador después de cada cosa que ejecuta (AREC_EV_*). */
	void (*on_exec)(void *ctx, int ev, uint64_t until);
};

enum { AREC_EV_SERVICE = 0, AREC_EV_BLOCK = 1, AREC_EV_STEP = 2 };

/* Estadísticas (se acumulan; las reinicia el dueño con memset). */
struct arec_stats {
	uint64_t enters;          /* entradas al código generado desde el despachador */
	uint64_t blocks_made;     /* bloques traducidos */
	uint64_t guest_made;      /* instrucciones guest traducidas */
	uint64_t host_words;      /* palabras PowerPC emitidas */
	uint64_t host_hot;        /* de esas, en la sección caliente (lo que corre sin caminos lentos) */
	uint64_t links;           /* enlaces parcheados */
	uint64_t interp_steps;    /* instrucciones que corrió el intérprete desde el despachador */
	uint64_t no_code;         /* de esas: código que no se puede traducir (fuera de RAM/ITCM/BIOS) */
	uint64_t callouts;        /* instrucciones interpretadas desde un bloque */
	uint64_t slow_rd, slow_wr;/* accesos por la ruta lenta */
	uint64_t code_wr;         /* escrituras que cayeron en una línea con código */
	uint64_t inval_blocks;    /* bloques invalidados por escrituras */
	uint64_t flushes;         /* vaciados enteros de la caché */
	uint64_t exits_pc, exits_link, exits_state, exits_budget; /* salidas al despachador por tipo */
	uint64_t mem_exits;       /* salidas a mitad de bloque después de un almacenamiento lento */
	uint64_t cold_flags;      /* banderas materializadas desde el CR en un camino frío */
	uint64_t compile_ns;      /* tiempo traduciendo (si hay reloj) */
	uint64_t bad_blocks;      /* bloques descartados por un error del análisis de banderas (deberían ser 0) */
	uint64_t call_by[AREC_CALL_KINDS]; /* llamadas al intérprete por clase (arec_call_kind_name) */
};

struct arec_pub {
	uint8_t *codemap;         /* por línea física de 64 B: 1 = puede haber código traducido */
	struct arec_stats st;
	uint64_t (*clock_ns)(void);
	/* Último bloque al que entró el despachador (diagnóstico: el hilo vigía los lee desde otro hilo). */
	volatile uint32_t last_pc;
	volatile uint64_t last_code;
};

/* Crea el recompilador para `c` (solo ARMv5: el ARM9). NULL si este host no ejecuta PowerPC64 o falta memoria. */
struct arec *arec_create(struct acore *c, const struct arec_cfg *cfg);
void arec_destroy(struct arec *j);
/* Corre la CPU hasta que c->cycles >= until (como acore_run). */
void arec_run(struct arec *j, uint64_t until);
/* Tira todo el código traducido (cambio de la vista de captación, reinicio). */
void arec_flush(struct arec *j);

/* Mapa de memoria de la ruta rápida: por página de 16 KB (`page` = dirección >> 14), el puntero de lectura y el
 * de escritura a los 16 KB del host (o NULL = ruta lenta) y el desplazamiento físico de la página de escritura
 * (AREC_PHYS_NONE si no hay código posible ahí). arec_map_begin borra todo; arec_map_end lo aplica. */
void arec_map_begin(struct arec *j);
void arec_map_set(struct arec *j, uint32_t page, uint8_t *rd, uint8_t *wr, uint32_t wr_phys);
void arec_map_end(struct arec *j);

/* Una escritura de `size` bytes (alineada, ≤ 4) en el desplazamiento físico `phys`: si toca código traducido,
 * lo invalida. Barata cuando no hay código ahí (una lectura del mapa). */
void arec_code_write_slow(struct arec *j, uint32_t phys, uint32_t size);
static inline void arec_code_write(struct arec *j, uint32_t phys, uint32_t size)
{
	const struct arec_pub *p = (const struct arec_pub *)(const void *)j;

	if (__builtin_expect(p->codemap[phys >> AREC_LINE_SHIFT] != 0, 0))
		arec_code_write_slow(j, phys, size);
}

static inline struct arec_stats *arec_stats(struct arec *j)
{
	return &((struct arec_pub *)(void *)j)->st;
}

/* Banderas (máscara NZCV = 8, 4, 2, 1) que la última salida del código generado dejó exactas en el CPSR; las demás
 * están muertas (se escriben antes de leerse) y pueden diferir del intérprete. Para --lockstep. */
unsigned arec_last_live(const struct arec *j);

/* Bloques vivos y bytes de código usados (para el log). */
void arec_usage(const struct arec *j, uint32_t *blocks, size_t *code_used, size_t *code_cap);

/* Pruebas: traduce el bloque en `pc` (con el bit 0 en Thumb) sin ejecutarlo; devuelve el número de palabras
 * PowerPC o < 0. Corre en cualquier host (no hace falta poder ejecutar). */
int arec_translate_only(struct acore *c, const struct arec_cfg *cfg, uint32_t pc, uint32_t *out, size_t cap);

#endif
