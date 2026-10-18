/*
 * SalsaDS — render 3D de la DS en un SPE (Fase 3.2). Trabajador de spukit: cada trabajo es un cuadro de geometría
 * (gx_frame en memoria principal) y la parte que le toca a este SPE (bandas alternas de 16 líneas); el rasterizador es
 * src/gpu3d/r3d_core.c, el mismo código que la referencia del PPE prueba en las pruebas unitarias y en la autoprueba
 * al arrancar. Dos SPE comparten un cuadro (docs/fase3.md, 3.2: reparto de SPE).
 *
 * Presupuesto del local store (256 KB; lo mide el enlace: `spu-size` y tools/spu-stack.py):
 *   contexto r3d_ctx ≈ 178 KB (182 656 B): color 2 × 18 KB (doble búfer de salida; el segundo, mientras se ordena, guarda los
 *   trozos de polígonos y el auxiliar del radix), profundidad 18 KB, atributos 18 KB, caché de texturas 64 bloques
 *   decodificados de 1 KB, líneas de VRAM 40 × 128 B, paleta convertida 1 KB, orden y extensiones de 2048 polígonos 16 KB,
 *   lista de la banda 4 KB, lotes de polígonos y vértices 2 × 11 KB;
 *   código ≈ 55 KB (la línea vectorial son cuatro variantes de ≈ 5-6 KB); la pila, lo que queda (mínimo 8 KB,
 *   raster3d_STACK_MIN).
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
