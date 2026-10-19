/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — Fase 0.4: E/S del núcleo en el PPE y el host (memcpy).
 *
 * Imita las reglas del MFC para que las pruebas del host las hagan cumplir:
 * cada transferencia queda pendiente en su etiqueta y se copia recién en
 * r3d_io_wait (si el núcleo lee un búfer antes de esperar su DMA, o lo
 * reescribe antes de que salga, la imagen sale mal); y las direcciones y
 * tamaños que no son múltiplos de 16, o los tamaños de más de 16 KB por
 * transferencia, se cuentan en r3d_io_mem_errors.
 */
#include <stdint.h>
#include <string.h>

#include "r3d.h"
#include "r3d_io.h"

#define MAX_PENDING 256
#define MAX_DMA 16384u

unsigned r3d_io_mem_errors;

struct pending {
	unsigned tag;
	int put;
	uint8_t *ls;
	uint64_t ea;
	uint32_t size;
};

static struct pending pend[MAX_PENDING];
static unsigned npend;

static void check(uint64_t a, uint64_t b, uint32_t size)
{
	if ((a | b | size) & 15u)
		r3d_io_mem_errors++;
}

static void push(unsigned tag, int put, void *ls, uint64_t ea, uint32_t size)
{
	if (npend == MAX_PENDING) {
		r3d_io_mem_errors++;
		return;
	}
	pend[npend++] = (struct pending){ tag, put, ls, ea, size };
}

void r3d_io_get(void *ls, uint64_t ea, uint32_t size, unsigned tag)
{
	check((uintptr_t)ls, ea, size);
	push(tag, 0, ls, ea, size);
}

void r3d_io_put(const void *ls, uint64_t ea, uint32_t size, unsigned tag)
{
	check((uintptr_t)ls, ea, size);
	push(tag, 1, (void *)ls, ea, size);
}

void r3d_io_gather(void *ls, uint64_t base, const uint16_t *idx, unsigned n, uint32_t elem, unsigned tag)
{
	if (n > R3D_CHUNK || elem > MAX_DMA)
		r3d_io_mem_errors++;
	for (unsigned k = 0; k < n; k++) {
		uint64_t ea = base + (uint64_t)idx[k] * elem;
		check((uintptr_t)ls, ea, elem);
		push(tag, 0, (uint8_t *)ls + (size_t)k * elem, ea, elem);
	}
}

void r3d_io_put_rows(const void *ls, uint64_t ea, unsigned rows, uint32_t row_bytes, uint32_t stride, unsigned tag)
{
	if (row_bytes > MAX_DMA)
		r3d_io_mem_errors++;
	for (unsigned r = 0; r < rows; r++) {
		check((uintptr_t)ls, ea + (uint64_t)r * stride, row_bytes);
		push(tag, 1, (uint8_t *)ls + (size_t)r * row_bytes, ea + (uint64_t)r * stride, row_bytes);
	}
}

void r3d_io_wait(uint32_t tag_mask)
{
	unsigned keep = 0;
	for (unsigned i = 0; i < npend; i++) {
		struct pending *p = &pend[i];
		if (!(tag_mask & (1u << p->tag))) {
			pend[keep++] = *p;
			continue;
		}
		if (p->put)
			memcpy((void *)(uintptr_t)p->ea, p->ls, p->size);
		else
			memcpy(p->ls, (const void *)(uintptr_t)p->ea, p->size);
	}
	npend = keep;
}

uint32_t r3d_io_ticks(void)
{
#if defined(__PPU__)
	uint64_t t;
	__asm__ volatile("mftb %0" : "=r"(t));
	return (uint32_t)t;
#else
	return 0;
#endif
}
