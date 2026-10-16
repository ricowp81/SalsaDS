/*
 * SalsaDS — Fase 0.5: composición 2D de las dos pantallas en un SPE.
 *
 * Trabajador de spukit: por cada trabajo trae el descriptor del cuadro por
 * DMA, compone la pantalla A y la B con gpu2d_compose_engine (src/gpu2d) y
 * devuelve cuánto tardó cada una.
 *
 * Presupuesto del local store (256 KB), medido al enlazar:
 *   gpu2d_ctx  OBJ 2 × 128 × 512 = 128 KB; teselas 4 × 2 × 2176 = 17 KB;
 *              cambios 3072 × 8 = 24 KB; resto (OAM, paletas, línea,
 *              mapas, listas) ~ 12 KB                           ~ 183 KB
 *   código + spukit + pila: lo que imprime spu-size y la holgura de la pila
 *   que verifica tools/spu-stack.py (mínimo del Makefile).
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "../job.h"

static gpu2d_ctx ctx;
static gpu2d_frame frame __attribute__((aligned(128)));

static int handler(void *user, const spukit_job *job, spukit_result *res)
{
	(void)user;
	if (job->kind != JOB_COMPOSE || job->size < sizeof(job_compose))
		return -1;
	const job_compose *jc = (const job_compose *)(const void *)job->data;
	job_result out;
	memset(&out, 0, sizeof out);
	out.ctx_bytes = sizeof ctx;

	spukit_dma_get(&frame, jc->frame_ea, sizeof frame, 0);
	spukit_dma_wait(1u << 0);
	for (int e = 0; e < 2; e++) {
		if (!(jc->engines & (1u << e)))
			continue;
		const uint32_t t0 = spukit_ticks();
		out.rc[e] = gpu2d_compose_engine(&ctx, &frame.eng[e]);
		out.ticks[e] = t0 - spukit_ticks();
		out.stats[e] = ctx.stats;
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
