/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SalsaDS — caché de código del JIT en la PS3. Ver rec_cache.h.
 * Adaptado de la prueba de la caché de Salsa64 (src/platform/ps3/rec_cache.c).
 */
#include "rec_cache.h"

#include <stdio.h>
#include <string.h>
#include <sys/systime.h>
#include <sys/thread.h>

#include "jit/ppcenc.h"
#include "ps3mapi.h"

#include <sys/memory.h>
#include <sys/process.h>

__asm__("	.section .salsads_rec_cache,\"ax\",@nobits\n"
	"	.balign 65536\n"
	"	.globl salsads_rec_cache_mem\n"
	"salsads_rec_cache_mem:\n"
	"	.space 5242880\n"  /* REC_CACHE_BYTES */
	"	.previous\n");
extern unsigned char salsads_rec_cache_mem[];
_Static_assert(REC_CACHE_BYTES == 5242880, "actualizar el .space de arriba");

void *salsads_rec_cache(void)
{
	return salsads_rec_cache_mem;
}

void salsads_rec_flush(const void *p, size_t n)
{
	uintptr_t a = (uintptr_t)p & ~(uintptr_t)127, e = (uintptr_t)p + n;
	for (uintptr_t x = a; x < e; x += 128)
		__asm__ volatile("dcbst 0,%0" : : "r"(x) : "memory");
	__asm__ volatile("sync" : : : "memory");
	for (uintptr_t x = a; x < e; x += 128)
		__asm__ volatile("icbi 0,%0" : : "r"(x) : "memory");
	__asm__ volatile("isync" : : : "memory");
}

static struct {
	volatile int done;
	volatile int ok;
	const char *marker, *tag;
	unsigned char *base;   /* zona que se prueba */
	size_t bytes;
	int zeroed;            /* 1: tiene que llegar en ceros (la cola NOBITS) */
	char why[128];
} probe;

static uint64_t probe_opd[3];

/* r3 = r3 * 3 + add; f(5) = 15 + add. */
static int write_and_call(volatile uint32_t *code, int add)
{
	code[0] = 0x1C630000u | 3u;                       /* mulli r3,r3,3 */
	code[1] = ppc_addi(3, 3, add);
	code[2] = PPC_BLR;
	salsads_rec_flush((const void *)code, 12);
	uint64_t toc;
	__asm__ volatile("mr %0,2" : "=r"(toc));
	probe_opd[0] = (uint64_t)(uintptr_t)code;
	probe_opd[1] = toc;
	probe_opd[2] = 0;
	__asm__ volatile("" : : "r"(probe_opd) : "memory");
	int (*fn)(int) = (int (*)(int))(void *)probe_opd;
	return fn(5);
}

static int marker_has(const char *marker, const char *tag)
{
	char line[96];
	FILE *f = fopen(marker, "r");
	if (!f)
		return 0;
	int hit = 0;
	if (fgets(line, sizeof line, f)) {
		line[strcspn(line, "\r\n")] = '\0';
		const char *v = !strncmp(line, "probando ", 9) ? line + 9 : !strncmp(line, "falló ", 7) ? line + 7 : NULL;
		hit = v && !strcmp(v, tag);
	}
	fclose(f);
	return hit;
}

static void marker_write(const char *what)
{
	FILE *f = fopen(probe.marker, "w");
	if (f) {
		fprintf(f, "%s %s\n", what, probe.tag);
		fclose(f);
	}
}

static void probe_thread(void *arg)
{
	(void)arg;
	volatile uint32_t *cache = (volatile uint32_t *)probe.base;
	volatile uint32_t *end = (volatile uint32_t *)(probe.base + probe.bytes - 128);
	int r;

	if (probe.zeroed && (cache[0] != 0 || end[0] != 0)) {
		snprintf(probe.why, sizeof probe.why, "la caché no llegó en ceros (0x%08x, 0x%08x)", (unsigned)cache[0],
			 (unsigned)end[0]);
		goto fail;
	}
	r = write_and_call(cache, 7);
	if (r != 22) {
		snprintf(probe.why, sizeof probe.why, "la ejecución dio %d en vez de 22", r);
		goto fail;
	}
	r = write_and_call(cache, 9);
	if (r != 24) {
		snprintf(probe.why, sizeof probe.why, "la reescritura dio %d en vez de 24 (caché de instrucciones vieja)", r);
		goto fail;
	}
	r = write_and_call(end, 4);
	if (r != 19) {
		snprintf(probe.why, sizeof probe.why, "la ejecución al final dio %d en vez de 19", r);
		goto fail;
	}
	for (size_t off = 0; off < probe.bytes; off += 65536) {
		volatile uint32_t *w = (volatile uint32_t *)(probe.base + off + 64);
		*w = *w;
	}
	for (int k = 0; k < 3; k++)
		cache[k] = end[k] = 0;
	salsads_rec_flush((const void *)cache, 12);
	salsads_rec_flush((const void *)end, 12);
	probe.ok = 1;
	goto out;
fail:
	probe.ok = 0;
out:
	probe.done = 1;
	sysThreadExit(0);
}

static enum rec_probe_status probe_zone(unsigned char *base, size_t bytes, int zeroed, const char *marker,
				       const char *tag, unsigned timeout_ms, char *why, size_t why_size, uint64_t *us)
{
	memset(&probe, 0, sizeof probe);
	probe.marker = marker;
	probe.tag = tag;
	probe.base = base;
	probe.bytes = bytes;
	probe.zeroed = zeroed;
	why[0] = '\0';
	if (marker_has(marker, tag)) {
		snprintf(why, why_size, "una prueba anterior cerró la aplicación o falló; borrar %s para repetirla", marker);
		return REC_PROBE_SKIPPED;
	}
	marker_write("probando");
	const uint64_t t0 = sysGetSystemTime();
	sys_ppu_thread_t tid;
	if (sysThreadCreate(&tid, probe_thread, NULL, 2000, 64 * 1024, THREAD_JOINABLE, "jit-probe") != 0) {
		snprintf(why, why_size, "no se pudo crear el hilo de prueba");
		marker_write("falló");
		return REC_PROBE_FAILED;
	}
	while (!probe.done && (sysGetSystemTime() - t0) / 1000 < timeout_ms)
		sysUsleep(500);
	*us = sysGetSystemTime() - t0;
	if (!probe.done) {
		snprintf(why, why_size, "el hilo de prueba no volvió en %u ms (CFW: la escritura en la caché cuelga el hilo)",
			 timeout_ms);
		marker_write("falló");
		return REC_PROBE_HUNG;
	}
	u64 rv;
	sysThreadJoin(tid, &rv);
	if (!probe.ok) {
		snprintf(why, why_size, "%s", probe.why);
		marker_write("falló");
		return REC_PROBE_FAILED;
	}
	remove(marker);
	return REC_PROBE_OK;
}

enum rec_probe_status salsads_rec_probe(const char *marker, const char *tag, unsigned timeout_ms, char *why,
					size_t why_size, uint64_t *us)
{
	return probe_zone(salsads_rec_cache_mem, REC_CACHE_BYTES, 1, marker, tag, timeout_ms, why, why_size, us);
}

/* Tabla de páginas de PROC_PAGE_ALLOCATE: una entrada por página (la primera es la zona). */
static u64 mapi_table[64];
static int mapi_held;
/* Con HEN 3.3.0 (PS3MAPI 0x124) las páginas que entrega PROC_PAGE_ALLOCATE siguen contando como libres para el
 * proceso: la siguiente reserva normal (el heap que crecía con las tablas del recompilador) recibía la misma memoria
 * física, y llenar esas tablas borraba las rutinas comunes (PS3 real, 2026-10-18: una marca escrita en 0x70080000
 * apareció en el heap en 0x55180000). Por eso, justo después, se pide la misma cantidad con sysMemoryAllocate y no
 * se toca nunca: absorbe esas páginas. Se comprueba página por página con una marca; si alguna no quedó absorbida,
 * no se usa PS3MAPI. */
static sys_mem_addr_t mapi_soak;

static int mapi_soak_pages(unsigned char *code, size_t bytes, char *why, size_t why_size)
{
	const size_t pages = bytes >> 20, off = 0x40;
	unsigned found = 0;

	if (sysMemoryAllocate(bytes, SYS_MEMORY_PAGE_SIZE_1M, &mapi_soak) != 0) {
		mapi_soak = 0;
		snprintf(why, why_size, "no hubo %u MB para apartar las páginas de PS3MAPI", (unsigned)(bytes >> 20));
		return -1;
	}
	for (size_t k = 0; k < pages; k++)
		*(volatile uint32_t *)(code + (k << 20) + off) = 0x5A1D0000u | (uint32_t)k;
	__asm__ volatile("sync" : : : "memory");
	for (size_t k = 0; k < pages; k++)
		for (size_t m = 0; m < pages; m++)
			if (*(const volatile uint32_t *)(uintptr_t)(mapi_soak + (m << 20) + off) == (0x5A1D0000u | (uint32_t)k)) {
				found++;
				break;
			}
	for (size_t k = 0; k < pages; k++)
		*(volatile uint32_t *)(code + (k << 20) + off) = 0;
	if (found != pages) {
		snprintf(why, why_size, "solo %u de %u páginas de PS3MAPI quedaron apartadas (las demás las comparte otra memoria)",
			 found, (unsigned)pages);
		sysMemoryFree(mapi_soak);
		mapi_soak = 0;
		return -1;
	}
	return 0;
}

void *salsads_rec_cache_mapi(size_t bytes, const char *marker, const char *tag, unsigned timeout_ms, char *why,
			     size_t why_size, uint64_t *us)
{
	s32 core = 0, rc;

	why[0] = '\0';
	*us = 0;
	if (bytes > sizeof mapi_table / sizeof mapi_table[0] * (1u << 20) || (bytes & 0xFFFFF)) {
		snprintf(why, why_size, "tamaño no válido (%u bytes)", (unsigned)bytes);
		return NULL;
	}
	if (marker_has(marker, tag)) {
		snprintf(why, why_size, "una prueba anterior cerró la aplicación o falló; borrar %s para repetirla", marker);
		return NULL;
	}
	if (!mapi_available(&core)) {
		snprintf(why, why_size, "PS3MAPI no disponible (0x%08x)", (unsigned)core);
		return NULL;
	}
	/* PROC_PAGE_ALLOCATE con páginas ejecutables (y PROC_PAGE_FREE con la tabla de dos direcciones) llegaron con el
	 * núcleo 0x0124 (Cobra 8.5, Mamba 8.4, HEN al día). Con núcleos anteriores puede faltar o, en Cobra 8.2, colgar la
	 * consola: no se intenta. */
	if (core < PS3MAPI_CORE_DYNAREC) {
		snprintf(why, why_size, "PS3MAPI %x es anterior al %x (páginas ejecutables)", (unsigned)core,
			 (unsigned)PS3MAPI_CORE_DYNAREC);
		return NULL;
	}
	/* La marca va antes de la reserva: si la llamada al núcleo congela la consola, el siguiente arranque no la repite
	 * (el hilo con plazo de la prueba no ayuda contra eso). */
	probe.marker = marker;
	probe.tag = tag;
	marker_write("probando");
	memset(mapi_table, 0, sizeof mapi_table);
	rc = mapi_page_alloc(sysProcessGetPid(), bytes, MAPI_PAGE_1M, 1, mapi_table);
	if (rc != 0 || !mapi_table[0]) {
		snprintf(why, why_size, "PS3MAPI %x: la reserva de %u MB falló (0x%08x)", (unsigned)core,
			 (unsigned)(bytes >> 20), (unsigned)rc);
		marker_write("falló");
		return NULL;
	}
	mapi_held = 1;
	if (mapi_soak_pages((unsigned char *)(uintptr_t)mapi_table[0], bytes, why, why_size) != 0) {
		salsads_rec_cache_mapi_free();
		marker_write("falló");
		return NULL;
	}
	remove(marker); /* la reserva volvió: probe_zone pone su propia marca (y no debe verla como de un arranque anterior) */
	if (probe_zone((unsigned char *)(uintptr_t)mapi_table[0], bytes, 0, marker, tag, timeout_ms, why, why_size, us) !=
	    REC_PROBE_OK) {
		/* si el hilo quedó colgado no se libera: la zona puede estar en uso */
		if (probe.done)
			salsads_rec_cache_mapi_free();
		return NULL;
	}
	snprintf(why, why_size, "PS3MAPI %x, %u MB en 0x%llx (apartados con otros %u MB en 0x%llx)", (unsigned)core,
		 (unsigned)(bytes >> 20), (unsigned long long)mapi_table[0], (unsigned)(bytes >> 20),
		 (unsigned long long)mapi_soak);
	return (void *)(uintptr_t)mapi_table[0];
}

void salsads_rec_cache_mapi_free(void)
{
	if (mapi_held) {
		mapi_page_free(sysProcessGetPid(), mapi_table);
		mapi_held = 0;
	}
	if (mapi_soak) {
		sysMemoryFree(mapi_soak);
		mapi_soak = 0;
	}
}

void salsads_rec_marker(const char *marker, const char *tag, int on)
{
	if (!on) {
		remove(marker);
		return;
	}
	FILE *f = fopen(marker, "w");
	if (f) {
		fprintf(f, "probando %s\n", tag);
		fclose(f);
	}
}
