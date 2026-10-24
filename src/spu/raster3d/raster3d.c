/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — render 3D de la DS en un SPE (Fase 3.2). Trabajador de spukit: cada trabajo es un cuadro de geometría
 * (gx_frame en memoria principal) y la parte que le toca a este SPE (bandas alternas de 16 líneas); el rasterizador es
 * src/gpu3d/r3d_core.c, el mismo código que la referencia del PPE prueba en las pruebas unitarias y en la autoprueba
 * al arrancar. Dos SPE comparten un cuadro (docs/fase3.md, 3.2: reparto de SPE).
 *
 * Presupuesto del local store (256 KB; lo mide el enlace: `spu-size` y tools/spu-stack.py):
 *   habitual: texto 60 808 B, datos 800 B, bss 184 480 B; pila máxima 7 392 B, holgura 8 432 B.
 *   XDR: texto 62 888 B, datos 800 B, bss 172 960 B; pila máxima 7 440 B, holgura 17 856 B.
 *   La variante XDR usa 448 celdas (28 KiB), cuatro vías, y conserva las líneas de VRAM y paletas.
 *   El programa habitual conserva 512 celdas y 64 bloques de 1 KiB, en memoria compartida entre modos.
 *   Dos programas elegidos al arrancar permiten reducir la reserva XDR sin encoger el modo apagado;
 *   reducir solo cell[] dentro de la unión no libera memoria, pues dominan los bloques. Mínimo: 8 192 B.
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "gpu3d/r3d_core.h"
#include "spu/raster3d/job.h"

_Static_assert(sizeof(job_r3d_result) <= 112, "el resultado de un trabajo de spukit es de 112 bytes");
_Static_assert(sizeof(r3d_job) <= 112, "los parámetros de un trabajo de spukit son de 112 bytes");

static r3d_ctx ctx;

static int handler(void *user, const spukit_job *job, spukit_result *res)
{
	job_r3d_result out;

	(void)user;
	memset(&out, 0, sizeof out);
	out.ctx_bytes = sizeof ctx;
	if (job->kind == JOB_RASTER3D && job->size >= sizeof(r3d_job)) {
		r3d_job j;

		memcpy(&j, job->data, sizeof j);
		out.rc = r3d_core_render(&ctx, &j, &out.st);
	} else if (job->kind == JOB_RASTER3D_PROBE) {
		uint32_t seed;

		memcpy(&seed, job->data, 4);
		for (uint32_t k = 0; k < R3D_NPROBES; k++)
			out.probe[k] = r3d_math_probe(seed, k);
	} else if (job->kind != JOB_RASTER3D_INFO) {
		return -1;
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
