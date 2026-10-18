/*
 * SalsaDS — E/S del rasterizador por bandas (Fase 3.2): DMA del MFC en el SPE; en el host y en el PPE, copias
 * inmediatas que comprueban las reglas del DMA (alineación a 16 y tamaños de 1, 2, 4, 8 o múltiplos de 16, hasta
 * 16 KB), para que una prueba del host atrape lo que en el SPE sería un error del bus.
 */
#ifndef SALSADS_GPU3D_R3D_IO_H
#define SALSADS_GPU3D_R3D_IO_H

#include <stdint.h>
#include <string.h>

#ifdef __SPU__
#include <spu_intrinsics.h>
#include <spu_mfcio.h>

typedef mfc_list_element_t r3d_lel;

#define R3D_T() spu_read_decrementer()
#define R3D_ACC(field, t0) ((field) += (uint32_t)((t0) - spu_read_decrementer()))

static inline void r3d_get(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	mfc_get(ls, ea, size, tag, 0, 0);
}
static inline void r3d_put(const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	mfc_put((volatile void *)ls, ea, size, tag, 0, 0);
}
/* Lista de DMA: elementos {size, eal} que comparten la mitad alta de `ea_hi`. */
static inline void r3d_getl(void *ls, uint64_t ea_hi, r3d_lel *list, uint32_t n, uint32_t tag)
{
	mfc_getl(ls, ea_hi, list, n * sizeof(r3d_lel), tag, 0, 0);
}
static inline void r3d_wait(uint32_t mask)
{
	mfc_write_tag_mask(mask);
	mfc_read_tag_status_all();
}
static inline void r3d_lel_set(r3d_lel *e, uint64_t ea, uint32_t size)
{
	e->notify = 0;
	e->reserved = 0;
	e->size = size;
	e->eal = (uint32_t)ea;
}
#else
typedef struct r3d_lel {
	uint32_t size;
	uint32_t eal;
	uint64_t ea; /* el host guarda la dirección completa */
} r3d_lel;

extern uint32_t r3d_io_errors; /* transferencias que el MFC rechazaría */

#define R3D_T() 0u
#define R3D_ACC(field, t0) ((void)(t0))

static inline void r3d_io_check(const void *ls, uint64_t ea, uint32_t size)
{
	const int small = size == 1 || size == 2 || size == 4 || size == 8;

	if (size == 0 || size > 16384u || (!small && (size & 15u)) ||
	    (!small && ((((uintptr_t)ls) & 15u) || (ea & 15u))) ||
	    (small && ((((uintptr_t)ls) & 15u) != (ea & 15u))))
		r3d_io_errors++;
}
static inline void r3d_get(void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	(void)tag;
	r3d_io_check(ls, ea, size);
	memcpy(ls, (const void *)(uintptr_t)ea, size);
}
static inline void r3d_put(const void *ls, uint64_t ea, uint32_t size, uint32_t tag)
{
	(void)tag;
	r3d_io_check(ls, ea, size);
	memcpy((void *)(uintptr_t)ea, ls, size);
}
static inline void r3d_getl(void *ls, uint64_t ea_hi, r3d_lel *list, uint32_t n, uint32_t tag)
{
	uint8_t *p = (uint8_t *)ls;

	(void)ea_hi;
	(void)tag;
	for (uint32_t i = 0; i < n; i++) {
		r3d_io_check(p, list[i].ea, list[i].size);
		memcpy(p, (const void *)(uintptr_t)list[i].ea, list[i].size);
		p += list[i].size;
	}
}
static inline void r3d_wait(uint32_t mask)
{
	(void)mask;
}
static inline void r3d_lel_set(r3d_lel *e, uint64_t ea, uint32_t size)
{
	e->size = size;
	e->eal = (uint32_t)ea;
	e->ea = ea;
}
#endif

#endif
