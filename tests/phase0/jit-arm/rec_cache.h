/*
 * SalsaDS — caché de código del JIT en la PS3 y su prueba con respaldo.
 *
 * La caché es una cola NOBITS al final del segmento de texto (mk/rec-cache.ld):
 * con HEN es lo único que se puede escribir y ejecutar; en RPCS3 el texto es
 * de solo lectura y en algunos CFW la primera escritura cuelga el hilo
 * (docs/cell-desde-salsa64.md §2.4). Por eso, antes de usarla, un hilo de
 * prueba la escribe, la ejecuta y la reescribe; si no vuelve en el tiempo
 * dado, o falla, el programa sigue con el intérprete. Un archivo marca la
 * prueba en curso: si la prueba mata el proceso, el siguiente arranque no la
 * repite.
 */
#ifndef SALSADS_JIT_ARM_REC_CACHE_H
#define SALSADS_JIT_ARM_REC_CACHE_H

#include <stddef.h>
#include <stdint.h>

#define REC_CACHE_BYTES (4u << 20)

void *salsads_rec_cache(void);
/* Vacía la caché de instrucciones para [p, p + n): dcbst, sync, icbi, isync. */
void salsads_rec_flush(const void *p, size_t n);

enum rec_probe_status {
	REC_PROBE_OK,
	REC_PROBE_FAILED,    /* el hilo terminó y algo salió mal (`why`) */
	REC_PROBE_HUNG,      /* el hilo no volvió dentro del plazo */
	REC_PROBE_SKIPPED,   /* una prueba anterior cerró la aplicación (marca en disco) */
};

/* Corre la prueba. `marker` es la ruta del archivo de marca; `tag` identifica
 * esta versión (otra versión repite la prueba). `timeout_ms` es el plazo. */
enum rec_probe_status salsads_rec_probe(const char *marker, const char *tag, unsigned timeout_ms, char *why,
					size_t why_size, uint64_t *us);

/* Caché grande pedida a PS3MAPI (fase52): `bytes` en páginas ejecutables de 1 MB de la syscall 8 de Cobra/Mamba/webMAN
 * (PROC_PAGE_ALLOCATE). En la PS3 real con HEN 4.91 se escriben directo y se ejecutan (Salsa64, docs/jit-ppe.md,
 * 2026-10-18). Hace la misma prueba que salsads_rec_probe sobre esa zona (con su propia marca `marker`). Devuelve la
 * zona o NULL (sin PS3MAPI, sin memoria, prueba fallida o saltada: `why`); entonces se usa la de salsads_rec_cache().
 * La zona queda fuera del segmento de texto: las llamadas del código generado a C van por mtctr/bctrl (el emisor lo
 * resuelve solo cuando un bl no llega). */
#define REC_MAPI_BYTES (16u << 20)
void *salsads_rec_cache_mapi(size_t bytes, const char *marker, const char *tag, unsigned timeout_ms, char *why,
			     size_t why_size, uint64_t *us);
/* Devuelve las páginas de salsads_rec_cache_mapi (al salir de la aplicación). */
void salsads_rec_cache_mapi_free(void);

/* Marca (on) o desmarca (off) el uso del código traducido: si el programa muere
 * mientras está marcada, el siguiente arranque con la misma versión no usa el JIT. */
void salsads_rec_marker(const char *marker, const char *tag, int on);

#endif
