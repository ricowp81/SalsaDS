/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Llamadas PS3MAPI (syscall 8 con el opcode 0x7777: la API de Cobra, Mamba y
 * webMAN) que usa la caché del recompilador (rec_cache.c); el mismo archivo
 * que la prueba del JIT de Salsa64 (tests/phase0/jit-ppe/ps3mapi.h), del mismo
 * autor. Solo los opcodes necesarios, reescritos; opcodes y orden de argumentos como en
 * github.com/bucanero/ps3mapi-lib (MIT) y github.com/crystalct/ps3mapidynarec
 * (GPLv2, include/ps3mapidyn.h). Resultados en la PS3 real: docs/estado.md (fase52).
 *
 * Con PS3MAPI ausente la syscall 8 devuelve un error negativo (o 0x80010003,
 * ENOSYS): mapi_available() lo distingue sin caer.
 */
#ifndef JIT_PS3MAPI_H
#define JIT_PS3MAPI_H

#include <ppu-lv2.h>
#include <ppu-types.h>
#include <stdint.h>

#define SC8_PS3MAPI              0x7777
#define PS3MAPI_GET_CORE_VERSION 0x0011
#define PS3MAPI_GET_FW_TYPE      0x0013
#define PS3MAPI_GET_FW_VERSION   0x0014
#define PS3MAPI_SET_PROC_MEM     0x0032
#define PS3MAPI_PAGE_ALLOCATE    0x0033
#define PS3MAPI_PAGE_FREE        0x0034
#define PS3MAPI_PAGE_FLAGS       0x2F   /* como webMAN MOD al cargar código en un juego */
#define PS3MAPI_CORE_MINVERSION  0x0120 /* el mínimo que exige ps3mapidynarec */
#define PS3MAPI_CORE_DYNAREC     0x0124 /* páginas ejecutables y PAGE_FREE con tabla (Cobra 8.5, Mamba 8.4) */

/* Códigos de tamaño de página (los mismos que sys_memory_allocate). */
#define MAPI_PAGE_4K  0x100
#define MAPI_PAGE_64K 0x200
#define MAPI_PAGE_1M  0x400

static inline s32 mapi_core_version(void)
{
	lv2syscall2(8, SC8_PS3MAPI, PS3MAPI_GET_CORE_VERSION);
	return_to_user_prog(s32);
}

/* PS3MAPI responde con una versión de núcleo positiva; sin él, error. */
static inline int mapi_available(s32 *core)
{
	s32 c = mapi_core_version();
	if (core)
		*core = c;
	return c > 0;
}

static inline s32 mapi_fw_type(char *buf)
{
	lv2syscall3(8, SC8_PS3MAPI, PS3MAPI_GET_FW_TYPE, (u64)(uintptr_t)buf);
	return_to_user_prog(s32);
}

static inline s32 mapi_fw_version(void)
{
	lv2syscall2(8, SC8_PS3MAPI, PS3MAPI_GET_FW_VERSION);
	return_to_user_prog(s32);
}

/* Escribe en la memoria del proceso aunque la página sea de solo lectura. */
static inline s32 mapi_set_mem(u32 pid, u64 addr, const void *buf, u32 size)
{
	lv2syscall6(8, SC8_PS3MAPI, PS3MAPI_SET_PROC_MEM, pid, addr, (u64)(uintptr_t)buf, size);
	return_to_user_prog(s32);
}

/* Reserva `size` bytes en páginas de `page_size` (MAPI_PAGE_*). `table` recibe
 * las direcciones de las páginas (table[0] es la zona) y debe tener sitio
 * para size / tamaño de página entradas. */
static inline s32 mapi_page_alloc(u32 pid, u64 size, u64 page_size, u64 executable, u64 *table)
{
	lv2syscall8(8, SC8_PS3MAPI, PS3MAPI_PAGE_ALLOCATE, pid, size, page_size, PS3MAPI_PAGE_FLAGS, executable,
		    (u64)(uintptr_t)table);
	return_to_user_prog(s32);
}

static inline s32 mapi_page_free(u32 pid, u64 *table)
{
	lv2syscall5(8, SC8_PS3MAPI, PS3MAPI_PAGE_FREE, pid, PS3MAPI_PAGE_FLAGS, (u64)(uintptr_t)table);
	return_to_user_prog(s32);
}

#endif
