/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — mezclador de sonido en un SPE (Fase 2.2).
 *
 * Trabajador de spukit: por cada cuadro de la DS trae el estado del mezclador (snd_state) y las escrituras a los registros
 * de sonido, genera las ~547 muestras del cuadro con snd_run (src/audio), las remuestrea a 48 kHz en flotante y devuelve
 * el estado, las muestras nativas de 16 bits y la salida. Los datos de los canales y los destinos de la captura se
 * leen y escriben por DMA directamente en la memoria emulada.
 *
 * Presupuesto del local store (256 KB): estado 4,5 KB + escrituras 32 KB + muestras nativas 2,6 KB + salida 8 KB +
 * pila de snd_run (muestras en flotante 5 KB, escalón de captura 2 KB) + código ≈ 30 KB: unos 85 KB. El enlace mide la
 * holgura de la pila (tools/spu-stack.py).
 */
#include <spukit/spu.h>
#include <sys/spu_thread.h>

#include "audio/snd.h"
#include "spu/snd/job.h"

_Static_assert(sizeof(snd_state) % 16 == 0, "snd_state debe ser múltiplo de 16 para el DMA");

static snd_state st __attribute__((aligned(128)));
static snd_ent log_buf[SND_LOG_MAX] __attribute__((aligned(128)));
static int16_t nat_buf[2 * SND_TICKS_MAX + 8] __attribute__((aligned(128)));
static float out_buf[2 * SND_OUT_MAX + 4] __attribute__((aligned(128)));
static uint8_t st_buf[1056 + 32] __attribute__((aligned(128)));
static uint8_t live_line[SND_LINE] __attribute__((aligned(128)));

static struct {
	uint64_t ea[4];
	uint32_t mask[4];
	int64_t live_delta;
	uint32_t verify, reads, changed;
	struct {
		uint32_t addr, len;
	} written[8];
	uint32_t nwritten, write_overflow;
} rgn;

static int captured_byte(uint32_t addr)
{
	for (uint32_t i = 0; i < rgn.nwritten; i++)
		if (addr - rgn.written[i].addr < rgn.written[i].len)
			return 1;
	return 0;
}

/* Una línea de SND_LINE bytes alineada de la memoria del ARM7. */
static void bus_fetch(void *ctx, uint32_t addr, uint8_t *dst)
{
	const uint32_t r = (addr >> 23) & 511u;

	(void)ctx;
	if (r >= 4 && r < 8 && rgn.ea[r - 4]) {
		spukit_dma_get(dst, rgn.ea[r - 4] + (addr & rgn.mask[r - 4]), SND_LINE, 0);
		if (rgn.verify)
			spukit_dma_get(live_line, (uint64_t)((int64_t)(rgn.ea[r - 4] + (addr & rgn.mask[r - 4])) +
							 rgn.live_delta), SND_LINE, 5);
		spukit_dma_wait((1u << 0) | (rgn.verify ? 1u << 5 : 0));
		if (rgn.verify) {
			int changed = 0;

			rgn.reads++;
			for (uint32_t i = 0; i < SND_LINE; i++)
				changed |= dst[i] != live_line[i] && !captured_byte(addr + i);
			rgn.changed += changed != 0;
		}
	} else {
		memset(dst, 0, SND_LINE);
	}
}

/* Escribe `n` bytes (de la captura) en una dirección alineada a 4: se lee el tramo de 16 bytes que los contiene, se
 * parchea y se devuelve (los vecinos no se tocan; el PPE puede escribirlos entre la lectura y la escritura, pero la
 * captura rara vez comparte líneas con algo vivo). */
static void bus_store(void *ctx, uint32_t addr, const uint8_t *src, uint32_t n)
{
	const uint32_t r = (addr >> 23) & 511u;
	uint64_t ea;
	uint64_t start;
	uint32_t len;

	(void)ctx;
	if (r < 4 || r >= 8 || !rgn.ea[r - 4])
		return;
	if (rgn.verify && rgn.nwritten < sizeof rgn.written / sizeof rgn.written[0]) {
		rgn.written[rgn.nwritten].addr = addr;
		rgn.written[rgn.nwritten].len = n;
		rgn.nwritten++;
	} else if (rgn.verify)
		rgn.write_overflow++;
	ea = rgn.ea[r - 4] + (addr & rgn.mask[r - 4]);
	start = ea & ~15ull;
	len = (uint32_t)(((ea + n + 15) & ~15ull) - start);
	if (len > sizeof st_buf - 32)
		return;
	spukit_dma_get(st_buf, start, len, 1);
	spukit_dma_wait(1u << 1);
	memcpy(st_buf + (ea - start), src, n);
	spukit_dma_put(st_buf, start, len, 1);
	spukit_dma_wait(1u << 1);
}

static int handler(void *user, const spukit_job *job, spukit_result *res)
{
	const job_snd *js;
	job_snd_result out;
	snd_bus bus = { NULL, bus_fetch, bus_store };
	uint32_t out_n = 0, t0, t1;

	(void)user;
	if (job->kind != JOB_SND || job->size < sizeof(job_snd))
		return -1;
	js = (const job_snd *)(const void *)job->data;
	memset(&out, 0, sizeof out);
	out.state_bytes = sizeof st;
	if (js->nlog > SND_LOG_MAX || js->nticks > SND_TICKS_MAX || js->out_max > SND_OUT_MAX)
		return -2;
	for (int i = 0; i < 4; i++) {
		rgn.ea[i] = js->rgn_ea[i];
		rgn.mask[i] = js->rgn_mask[i];
	}
	rgn.live_delta = js->live_delta;
	rgn.verify = js->verify;
	rgn.reads = rgn.changed = 0;
	rgn.nwritten = 0;
	rgn.write_overflow = 0;
	spukit_dma_get(&st, js->state_ea, sizeof st, 2);
	spukit_dma_wait(1u << 2);
	st.mode = js->mode;
	st.ratio_adj = js->ratio_adj;
	if (js->nlog)
		spukit_dma_get(log_buf, js->log_ea, (js->nlog * (uint32_t)sizeof(snd_ent) + 15u) & ~15u, 3);
	spukit_dma_wait((1u << 2) | (1u << 3));

	t0 = spukit_ticks();
	out.applied = snd_run(&st, log_buf, js->nlog, js->nticks, &bus, nat_buf, out_buf, js->out_max, &out_n);
	t1 = spukit_ticks();
	out.mix_ticks = t0 - t1;
	out.out_n = out_n;
	out.busy = st.busy;
	out.peak = st.peak;
	out.ram_reads = rgn.reads;
	out.ram_changed = rgn.changed;
	out.write_overflow = rgn.write_overflow;

	spukit_dma_put(&st, js->state_ea, sizeof st, 2);
	spukit_dma_put(nat_buf, js->nat_ea, (js->nticks * 4u + 15u) & ~15u, 3);
	if (out_n)
		spukit_dma_put(out_buf, js->out_ea, (out_n * 8u + 15u) & ~15u, 4);
	spukit_dma_wait((1u << 2) | (1u << 3) | (1u << 4));
	out.ticks = t0 - spukit_ticks();
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
