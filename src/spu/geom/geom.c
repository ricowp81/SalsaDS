/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — motor de geometría de la DS en un SPE (Fase 3.5). Trabajador de spukit: cada trabajo es un trozo del registro de
 * órdenes que el motor de tiempo del PPE (src/gpu3d/gx.c) fue anotando al empezar cada orden; aquí se ejecutan con el mismo
 * código que el modo directo del PPE (src/gpu3d/gx_exec.c, el mismo bit a bit) y los vértices y polígonos salen por DMA al
 * cuadro de salida en memoria principal. El contexto (matrices, pilas, estado de vértices y luces) vive aquí entre trabajos.
 *
 * Presupuesto del local store (256 KB; lo mide el enlace: `spu-size` y tools/spu-stack.py):
 *   contexto gx_exec ≈ 6 KB; ventana de vértices 512 × 32 B = 16 KB, polígonos 256 × 48 B = 12 KB,
 *   tramo del registro 4096 palabras = 16 KB y búfer DMA ≈ 16 KB. La caché de proyección vive en los cuatro vértices
 *   del ensamblador (40 bytes extra por vértice más alineación); no amplía las ventanas ni cambia las transferencias.
 *   Enlace: texto 42 344 B, datos 800 B, bss 68 992 B; pila disponible 149 872 B, cadena máxima 5600 B,
 *   holgura 144 272 B (mínimo exigido 8192 B). La ruta de recorte también cuenta en esa cadena.
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "gpu3d/gx_exec.h"
#include "spu/geom/job.h"

_Static_assert(sizeof(job_geom) <= 112, "los parámetros de un trabajo de spukit son de 112 bytes");
_Static_assert(sizeof(job_geom_result) <= 112, "el resultado de un trabajo de spukit es de 112 bytes");
_Static_assert(sizeof(gx_exec_out) % 16 == 0, "gx_exec_out se sube por DMA");
_Static_assert(sizeof(gx_vertex) % 16 == 0 && sizeof(gx_poly) % 16 == 0, "los vértices y polígonos se suben por DMA");

#define VCAP 512u
#define PCAP 256u
#define CHUNK 4096u
#define TAG_PUT 1u
#define TAG_GET 2u

static gx_exec ex;
static gx_vertex vstage[VCAP] __attribute__((aligned(128)));
static gx_poly pstage[PCAP] __attribute__((aligned(128)));
static uint32_t chunk[CHUNK] __attribute__((aligned(128)));       /* tramo del registro ya alineado a palabra */
static uint32_t dmabuf[CHUNK + 8] __attribute__((aligned(128)));  /* destino del DMA (alineado a 16): el tramo cae con su desplazamiento */
static gx_exec_out out_ls __attribute__((aligned(128)));

typedef struct io_state {
	uint64_t log_ea, vtx_ea, poly_ea;
	uint32_t puts;
} io_state;

/* trae `n` palabras del registro desde la palabra `pos` (dirección alineada a 4, no a 16) */
static void fetch(void *ctx, uint32_t pos, uint32_t *dst, uint32_t n)
{
	const io_state *io = ctx;
	const uint64_t ea = io->log_ea + 4ull * pos;
	const uint64_t a = ea & ~15ull;
	const uint32_t off = (uint32_t)(ea - a), bytes = (n * 4u + off + 15u) & ~15u;

	spukit_dma_get(dmabuf, a, bytes, TAG_GET);
	spukit_dma_wait(1u << TAG_GET);
	memcpy(dst, (const uint8_t *)dmabuf + off, (size_t)n * 4u);
}

static void put_verts(void *ctx, uint32_t first, const gx_vertex *src, uint32_t n)
{
	io_state *io = ctx;

	spukit_dma_put(src, io->vtx_ea + (uint64_t)first * sizeof(gx_vertex), n * (uint32_t)sizeof(gx_vertex), TAG_PUT);
	spukit_dma_wait(1u << TAG_PUT); /* la ventana se reutiliza */
	io->puts++;
}

static void put_polys(void *ctx, uint32_t first, const gx_poly *src, uint32_t n)
{
	io_state *io = ctx;

	spukit_dma_put(src, io->poly_ea + (uint64_t)first * sizeof(gx_poly), n * (uint32_t)sizeof(gx_poly), TAG_PUT);
	spukit_dma_wait(1u << TAG_PUT);
	io->puts++;
}

static int handler(void *user, const spukit_job *job, spukit_result *res)
{
	job_geom_result out;

	(void)user;
	memset(&out, 0, sizeof out);
	out.ctx_bytes = (uint32_t)sizeof ex;
	if (job->kind == JOB_GEOM && job->size >= sizeof(job_geom)) {
		job_geom j;
		io_state io;
		gx_job_io gio;
		uint64_t before = 0, after = 0;

		memcpy(&j, job->data, sizeof j);
		io.log_ea = j.log_ea;
		io.vtx_ea = j.vtx_ea;
		io.poly_ea = j.poly_ea;
		io.puts = 0;
		gio.ctx = &io;
		gio.fetch = fetch;
		gio.put_verts = put_verts;
		gio.put_polys = put_polys;
		uint32_t pbefore[5] = { 0, 0, 0, 0, 0 };

		if (!(j.flags & GX_JOB_RESET)) {
			for (unsigned c = 0; c < 0x80; c++)
				before += ex.by_cmd[c];
			memcpy(pbefore, ex.prof, sizeof pbefore);
		}
		gx_job_run(&ex, j.flags, j.nwords, &gio, vstage, VCAP, pstage, PCAP, chunk, CHUNK);
		for (unsigned c = 0; c < 0x80; c++)
			after += ex.by_cmd[c];
		out.cmds = (uint32_t)(after - before);
		for (unsigned k = 0; k < 5; k++)
			out.prof[k] = ex.prof[k] - pbefore[k];
		out.puts = io.puts;
		out_ls = ex.out;
		spukit_dma_put(&out_ls, j.out_ea, (uint32_t)sizeof out_ls, TAG_PUT);
		spukit_dma_wait(1u << TAG_PUT);
	} else if (job->kind != JOB_GEOM_INFO) {
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
