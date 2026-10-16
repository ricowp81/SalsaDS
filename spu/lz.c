/* SPDX-License-Identifier: MIT */
/*
 * spukit — programa SPU del compresor LZ (spukit/lz.h): un trabajador que
 * comprime bloques de hasta 64 KB de memoria principal a memoria principal.
 *
 * Presupuesto de local store (256 KB):
 *   código (compresor, bucle de spukit)   ~ 3 KB
 *   bloque de entrada                     64 KB
 *   bloque de salida                      64 KB + 0,3 KB
 *   tabla de dispersión                   16 KB
 *   spukit (descriptor, resultado…)       < 1 KB
 *   pila                                  el resto (~100 KB)
 *
 * Etiqueta DMA 0: entrada y salida (se espera entre una y otra).
 */
#include <spukit/lzjob.h>
#include <spukit/spu.h>
#include <sys/spu_thread.h>

_Static_assert(sizeof(struct spukit_lz_job) <= SPUKIT_DATA_SIZE, "spukit_lz_job no cabe en el descriptor");
_Static_assert(sizeof(struct spukit_lz_result) <= SPUKIT_RESULT_SIZE, "spukit_lz_result no cabe en el resultado");

#define TAG 0u

static uint8_t in_buf[SPUKIT_LZ_BLOCK] __attribute__((aligned(128)));
static uint8_t out_buf[SPUKIT_LZ_BOUND(SPUKIT_LZ_BLOCK) + 32] __attribute__((aligned(128)));
static uint16_t table[SPUKIT_LZ_TABLE_ENTRIES] __attribute__((aligned(16)));

static int handler(void *ctx, const spukit_job *j, spukit_result *res)
{
	(void)ctx;
	struct spukit_lz_job job;
	struct spukit_lz_result r;
	if (j->kind != SPUKIT_LZ_JOB_COMPRESS)
		return SPUKIT_LZ_ERR_KIND;
	memcpy(&job, j->data, sizeof job);
	if (job.len > SPUKIT_LZ_BLOCK || ((job.src_ea | job.dst_ea) & 15) || job.cap < SPUKIT_LZ_BOUND(job.len) + 16)
		return SPUKIT_LZ_ERR_ARG;
	r.clen = 0;
	if (job.len) {
		uint32_t padded = (job.len + 15u) & ~15u;
		spukit_dma_get(in_buf, job.src_ea, padded, TAG);
		spukit_dma_wait(1u << TAG);
		size_t c = spukit_lz_compress(in_buf, job.len, out_buf, sizeof out_buf, table);
		if (c && c < job.len) {
			spukit_dma_put(out_buf, job.dst_ea, ((uint32_t)c + 15u) & ~15u, TAG);
			spukit_dma_wait(1u << TAG);
			r.clen = (uint32_t)c;
		}
	}
	memcpy(res->data, &r, sizeof r);
	return 0;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg, (void)a2, (void)a3;
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
