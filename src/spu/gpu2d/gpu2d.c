/*
 * SalsaDS — composición 2D de una pantalla en un SPE (Fase 0.5, completada en la 2.1).
 *
 * Trabajador de spukit: por cada trabajo trae el descriptor de la pantalla por DMA, la compone con
 * gpu2d_compose_engine (src/gpu2d) y devuelve cuánto tardó y sus contadores. Un SPE por pantalla (la decisión del
 * cierre de la 0.5): el PPE reparte A y B en dos trabajadores.
 *
 * Presupuesto del local store (256 KB): ver docs/memory-budget.md; el programa imprime sizeof(gpu2d_ctx) y el
 * enlace mide la holgura de la pila (tools/spu-stack.py).
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "spu/gpu2d/job.h"

static gpu2d_ctx ctx;
static gpu2d_engine eng __attribute__((aligned(16)));

static int handler(void *user, const spukit_job *job, spukit_result *res)
{
	(void)user;
	if (job->kind != JOB_COMPOSE || job->size < sizeof(job_compose))
		return -1;
	const job_compose *jc = (const job_compose *)(const void *)job->data;
	job_result out;
	memset(&out, 0, sizeof out);
	out.ctx_bytes = sizeof ctx;

	spukit_dma_get(&eng, jc->frame_ea + (uint64_t)jc->engine * sizeof(gpu2d_engine), sizeof eng, 0);
	spukit_dma_wait(1u << 0);
	{
		const uint32_t t0 = spukit_ticks();
		out.rc = gpu2d_compose_engine(&ctx, &eng);
		out.ticks = t0 - spukit_ticks();
		out.stats = ctx.stats;
	}
	memcpy(res->data, &out, sizeof out);
	return 0;
}

int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
{
	(void)arg;
	(void)a2;
	(void)a3;
	spukit_run(ctrl_ea, handler, NULL);
	spu_thread_exit(0);
	return 0;
}
