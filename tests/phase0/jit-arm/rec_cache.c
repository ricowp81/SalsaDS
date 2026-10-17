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

__asm__("	.section .salsads_rec_cache,\"ax\",@nobits\n"
	"	.balign 65536\n"
	"	.globl salsads_rec_cache_mem\n"
	"salsads_rec_cache_mem:\n"
	"	.space 4194304\n"  /* REC_CACHE_BYTES */
	"	.previous\n");
extern unsigned char salsads_rec_cache_mem[];
_Static_assert(REC_CACHE_BYTES == 4194304, "actualizar el .space de arriba");

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
	volatile uint32_t *cache = (volatile uint32_t *)salsads_rec_cache_mem;
	volatile uint32_t *end = (volatile uint32_t *)(salsads_rec_cache_mem + REC_CACHE_BYTES - 128);
	int r;

	if (cache[0] != 0 || end[0] != 0) {
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
	for (size_t off = 0; off < REC_CACHE_BYTES; off += 65536) {
		volatile uint32_t *w = (volatile uint32_t *)(salsads_rec_cache_mem + off + 64);
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

enum rec_probe_status salsads_rec_probe(const char *marker, const char *tag, unsigned timeout_ms, char *why,
					size_t why_size, uint64_t *us)
{
	memset(&probe, 0, sizeof probe);
	probe.marker = marker;
	probe.tag = tag;
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
