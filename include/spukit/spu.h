/* SPDX-License-Identifier: MIT */
/*
 * spukit — lado SPU (solo cabecera): bucle de trabajos y ayudas de DMA.
 *
 *     static int handler(void *ctx, const spukit_job *job, spukit_result *res)
 *     {
 *         ... DMA de entrada, cálculo, DMA de salida ...
 *         return 0;                 // va a res->error
 *     }
 *
 *     int main(uint64_t ctrl_ea, uint64_t arg, uint64_t a2, uint64_t a3)
 *     {
 *         spukit_run(ctrl_ea, handler, NULL);
 *         spu_thread_exit(0);
 *         return 0;
 *     }
 *
 * spukit_run duerme en la mailbox de entrada hasta el timbre del PPE,
 * atiende todos los trabajos publicados (uno detrás de otro) y vuelve a
 * dormir; termina cuando el PPE pide salir. Antes de marcar un trabajo como
 * terminado espera todas las etiquetas DMA: lo que el manejador haya
 * escrito con DMA es visible para el PPE cuando ve status.done.
 *
 * La etiqueta DMA SPUKIT_TAG (31) es del bucle; el manejador puede usar
 * las demás.
 */
#ifndef SPUKIT_SPU_H
#define SPUKIT_SPU_H

#include <spu_intrinsics.h>
#include <spu_mfcio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "spukit/queue.h"

#define SPUKIT_TAG     31u
#define SPUKIT_DMA_MAX 16384u

typedef int (*spukit_handler)(void *ctx, const spukit_job *job, spukit_result *res);

static inline void spukit_dma_wait(uint32_t tag_mask)
{
	mfc_write_tag_mask(tag_mask);
	mfc_read_tag_status_all();
}

/* get/put de cualquier tamaño múltiplo de 16 (en trozos de 16 KB); ls y ea
 * alineados a 16. No espera. */
static inline void spukit_dma_get(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	uint8_t *p = (uint8_t *)ls;
	while (size) {
		uint32_t n = size < SPUKIT_DMA_MAX ? size : SPUKIT_DMA_MAX;
		mfc_get(p, ea, n, tag, 0, 0);
		p += n;
		ea += n;
		size -= n;
	}
}

static inline void spukit_dma_put(const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	const uint8_t *p = (const uint8_t *)ls;
	while (size) {
		uint32_t n = size < SPUKIT_DMA_MAX ? size : SPUKIT_DMA_MAX;
		mfc_put((volatile void *)p, ea, n, tag, 0, 0);
		p += n;
		ea += n;
		size -= n;
	}
}

/* Trae `size` bytes desde una dirección cualquiera: copia el tramo
 * alineado a 16 que los contiene en `buf` (de al menos size + 32 bytes,
 * alineado a 16) y devuelve el puntero al primer byte pedido. Espera. */
static inline void *spukit_get_unaligned(void *buf, uint64_t ea, uint32_t size, uint32_t tag)
{
	uint64_t start = ea & ~15ull;
	uint32_t len = (uint32_t)(((ea + size + 15) & ~15ull) - start);
	spukit_dma_get(buf, start, len, tag);
	spukit_dma_wait(1u << tag);
	return (uint8_t *)buf + (ea - start);
}

static inline uint32_t spukit_ticks(void)
{
	return spu_read_decrementer();
}

static inline int spukit_run(uint64_t ctrl_ea, spukit_handler fn, void *ctx)
{
	static spukit_head head __attribute__((aligned(128)));
	static spukit_job job __attribute__((aligned(128)));
	static spukit_result res __attribute__((aligned(128)));
	static spukit_status st __attribute__((aligned(128)));
	const uint64_t head_ea = ctrl_ea + offsetof(spukit_ctrl, head);
	const uint64_t status_ea = ctrl_ea + offsetof(spukit_ctrl, status);
	uint32_t done = 0, wakeups = 0;

	spu_write_decrementer(0xFFFFFFFFu);
	for (;;) {
		(void)spu_read_in_mbox();  /* duerme hasta el timbre */
		wakeups++;
		for (;;) {
			mfc_get(&head, head_ea, sizeof head, SPUKIT_TAG, 0, 0);
			spukit_dma_wait(1u << SPUKIT_TAG);
			if (head.posted == done)
				break;
			uint32_t seq = done + 1 ? done + 1 : 1;
			uint32_t slot = SPUKIT_SLOT(seq);
			mfc_get(&job, ctrl_ea + offsetof(spukit_ctrl, jobs) + slot * sizeof job, sizeof job,
				SPUKIT_TAG, 0, 0);
			spukit_dma_wait(1u << SPUKIT_TAG);

			memset(&res, 0, sizeof res);
			res.seq = seq;
			uint32_t t0 = spukit_ticks();
			res.error = job.seq == seq ? fn(ctx, &job, &res) : -1;
			res.ticks = t0 - spukit_ticks();

			mfc_put(&res, ctrl_ea + offsetof(spukit_ctrl, results) + slot * sizeof res, sizeof res,
				SPUKIT_TAG, 0, 0);
			spukit_dma_wait(0xFFFFFFFFu);  /* todo lo del manejador y el resultado */
			st.done = seq;
			st.wakeups = wakeups;
			mfc_put(&st, status_ea, sizeof st, SPUKIT_TAG, 0, 0);
			spukit_dma_wait(1u << SPUKIT_TAG);
			done = seq;
		}
		if (head.quit)
			return 0;
	}
}

#endif
