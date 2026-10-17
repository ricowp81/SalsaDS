/*
 * SalsaDS — Fase 0.4: programa SPU del rasterizador (trabajador de spukit).
 *
 * Un trabajo = la parte de un cuadro que le toca a este SPE (struct r3d_job):
 * r3d_frame (r3d_core.c) con la E/S por DMA del MFC de aquí. Devuelve las
 * struct r3d_stats en el resultado de spukit.
 *
 * Etiquetas DMA (r3d_core.c): 0 y 1 lotes de triángulos y de registros (doble
 * búfer), 2 y 3 registros de setup que salen y color de cada unidad (doble
 * búfer), 4 texturas y paletas. La 31 es de spukit.
 *
 * Presupuesto de local store (256 KB), medido al enlazar (spu-size y
 * tools/spu-stack.py, con raster_STACK_MIN = 8 KB en el Makefile):
 *   código (núcleo con 6 variantes del dibujo, spukit, libc)  27,3 KB
 *   datos                                                      0,8 KB
 *   bss                                                      202,1 KB
 *     caché de texturas: 28 páginas de 4 KB                   112 KB
 *     color de la unidad × 2 (4096 píxeles + 16 B)             32 KB
 *     profundidad de la unidad (24 bits en 32)                 16 KB
 *     cajas de los triángulos en unidades (4 × 2048 × 2 B)     16 KB
 *     registros de setup × 2 lotes (2 × 32 × 192 B)            12 KB
 *     triángulos de entrada × 2 lotes (2 × 32 × 64 B)           4 KB
 *     lista de la unidad (2048 × 2 B)                           4 KB
 *     VRAM en tránsito, paleta decodificada, listas de DMA,
 *     spukit                                                  ~6 KB
 *   pila: 25,6 KB libres; la cadena más honda (_start → main →
 *   r3d_frame → setup) usa 2,5 KB: holgura 23,1 KB
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "r3d.h"
#include "r3d_core.h"
#include "r3d_io.h"

/* Una lista de DMA por etiqueta de lote (0 y 1) y por etiqueta de salida
 * (2 y 3): cada una se reescribe solo después de esperar su etiqueta. */
static mfc_list_element_t gather_list[2][R3D_CHUNK] __attribute__((aligned(16)));
static mfc_list_element_t rows_list[2][16] __attribute__((aligned(16)));

void r3d_io_get(void *ls, uint64_t ea, uint32_t size, unsigned tag) { spukit_dma_get(ls, ea, size, tag); }

void r3d_io_put(const void *ls, uint64_t ea, uint32_t size, unsigned tag) { spukit_dma_put(ls, ea, size, tag); }

void r3d_io_gather(void *ls, uint64_t base, const uint16_t *idx, unsigned n, uint32_t elem, unsigned tag)
{
	mfc_list_element_t *l = gather_list[tag & 1u];
	const uint32_t lo = (uint32_t)base;  /* la lista lleva los 32 bits bajos; los altos, base */
	for (unsigned k = 0; k < n; k++) {
		l[k].notify = 0;
		l[k].reserved = 0;
		l[k].size = elem;
		l[k].eal = lo + idx[k] * elem;
	}
	mfc_getl(ls, base, l, n * sizeof *l, tag, 0, 0);
}

void r3d_io_put_rows(const void *ls, uint64_t ea, unsigned rows, uint32_t row_bytes, uint32_t stride, unsigned tag)
{
	mfc_list_element_t *l = rows_list[tag & 1u];
	const uint32_t lo = (uint32_t)ea;
	for (unsigned r = 0; r < rows; r++) {
		l[r].notify = 0;
		l[r].reserved = 0;
		l[r].size = row_bytes;
		l[r].eal = lo + r * stride;
	}
	mfc_putl((volatile void *)ls, ea, l, rows * sizeof *l, tag, 0, 0);
}

void r3d_io_wait(uint32_t tag_mask) { spukit_dma_wait(tag_mask); }

/* spukit pone el decrementador en 0xFFFFFFFF al arrancar y baja a la
 * frecuencia del timebase. */
uint32_t r3d_io_ticks(void) { return 0u - spukit_ticks(); }

static int handler(void *ctx, const spukit_job *job, spukit_result *res)
{
	(void)ctx;
	struct r3d_job j;
	struct r3d_stats st;
	if (job->kind == R3D_KIND_SELFTEST) {
		struct r3d_selftest t __attribute__((aligned(16)));
		r3d_selftest(&t);
		memcpy(res->data, &t, sizeof t);
		return 0;
	}
	if (job->kind != R3D_KIND_FRAME)
		return -2;
	memcpy(&j, job->data, sizeof j);
	int e = r3d_frame(&j, &st);
	memcpy(res->data, &st, sizeof st);
	return e;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg, (void)a2, (void)a3;
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
